#include "picoquic_internal.h"
#include "plugin.h"
#include "../helpers.h"
#include "bpf.h"

static uint8_t* frames_uint8_decode(uint8_t* bytes, const uint8_t* bytes_max, uint8_t* n)
{
    if (bytes < bytes_max) {
        *n = *bytes++;
    } else {
        bytes = NULL;
    }
    return bytes;
}

/**
 * The interface for the decode_frame protocol operation is the same for all:
 * uint8_t* bytes = cnx->protoop_inputv[0]
 * const uint8_t* bytes_max = cnx->protoop_inputv[1]
 * uint64_t current_time = cnx->protoop_inputv[2]
 * int epoch = cnx->protoop_inputv[3]
 * int ack_needed = cnx->protoop_inputv[4]
 *
 * Output: uint8_t* bytes
 * cnx->protoop_outputv[0] = ack_needed
 */
protoop_arg_t decode_mp_new_connection_id_frame(picoquic_cnx_t* cnx)
{
    uint8_t* bytes = (uint8_t *) cnx->protoop_inputv[0];
    const uint8_t* bytes_max = (const uint8_t *) cnx->protoop_inputv[1];
    uint64_t current_time = (uint64_t) cnx->protoop_inputv[2];
    int ack_needed = (int) cnx->protoop_inputv[4];

    ack_needed = 1;

    uint64_t path_id, seq;
    size_t path_id_l, seq_l;
    size_t byte_index = 1; /* Skip the first byte */
    uint8_t stateless_reset_token[16];
    int new_path_index = 0;
    /* Store data */
    bpf_data *bpfd = get_bpf_data(cnx);

    /* TODO: store the connection ID in order to support migration. */
    if (bytes_max - bytes > byte_index) {
        path_id_l = picoquic_varint_decode(bytes + byte_index, (size_t)(bytes_max - (bytes + byte_index)), &path_id);
        byte_index += path_id_l;
    }

    /* Find the path index in the array */
    int path_index = mp_get_path_index(bpfd, path_id, &new_path_index);

    bpfd->paths[path_index].path_id = path_id;

    if (bytes_max - bytes > byte_index) {
        seq_l = picoquic_varint_decode(bytes + byte_index, (size_t)(bytes_max - (bytes + byte_index)), &seq);
        byte_index += seq_l;
    }
    if (bytes_max - bytes > byte_index) {
        frames_uint8_decode(bytes + byte_index, bytes_max, &bpfd->paths[path_index].remote_cnxid.id_len);
        byte_index++;
    }
    if (bytes_max - bytes > byte_index) {
        for (int i = 0; i < bpfd->paths[path_index].remote_cnxid.id_len; i++) {
            frames_uint8_decode(bytes + byte_index + i, bytes_max, &bpfd->paths[path_index].remote_cnxid.id[i]);
        }
        byte_index += bpfd->paths[path_index].remote_cnxid.id_len;
    }

    if (bytes_max - bytes > byte_index) {
        for (int i = 0; i < 16; i++) {
            frames_uint8_decode(bytes + byte_index + i, bytes_max, &bpfd->paths[path_index].reset_secret[i]);
        }
        byte_index += 16;
    }

    bpfd->nb_proposed_rcv++;

    if (!new_path_index && bpfd->paths[path_index].local_cnxid.id_len > 0) {
        mp_path_ready(cnx, &bpfd->paths[path_index], current_time);
    } else {
        bpfd->paths[path_index].state = 0;
    }

    cnx->protoop_outputc_callee = 1;
    cnx->protoop_outputv[0] = ack_needed;
    return (protoop_arg_t) bytes + byte_index;
}