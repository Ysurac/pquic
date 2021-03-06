#include "bpf.h"

/**
 * See PROTOOP_NOPARAM_UPDATE_RTT
 */
protoop_arg_t update_rtt(picoquic_cnx_t *cnx)
{
    uint64_t largest = (uint64_t) get_cnx(cnx, AK_CNX_INPUT, 0);
    uint64_t current_time = (uint64_t) get_cnx(cnx, AK_CNX_INPUT, 1);
    uint64_t ack_delay = (uint64_t) get_cnx(cnx, AK_CNX_INPUT, 2);
    picoquic_packet_context_enum pc = (picoquic_packet_context_enum) get_cnx(cnx, AK_CNX_INPUT, 3);
    picoquic_path_t *sending_path = (picoquic_path_t *) get_cnx(cnx, AK_CNX_INPUT, 4);
    picoquic_path_t *receiving_path = (picoquic_path_t *) get_cnx(cnx, AK_CNX_INPUT, 5);
    int receiving_uniflow_index = -1;
    int is_new_ack = 0;

    bpf_data *bpfd = get_bpf_data(cnx);
    bpf_tuple_data *bpftd = get_bpf_tuple_data(cnx);

    if (receiving_path != NULL) {
        receiving_uniflow_index = mp_get_uniflow_index_from_path(bpfd, false, receiving_path);
    }

    picoquic_packet_context_t * pkt_ctx = (picoquic_packet_context_t *) get_path(sending_path, AK_PATH_PKT_CTX, pc);
    picoquic_packet_t* packet = (picoquic_packet_t *) get_pkt_ctx(pkt_ctx, AK_PKTCTX_RETRANSMIT_NEWEST);

    /* Check whether this is a new acknowledgement */
    uint64_t highest_acknowledged = (uint64_t) get_pkt_ctx(pkt_ctx, AK_PKTCTX_HIGHEST_ACKNOWLEDGED);
    picoquic_sack_item_t *first_sack = (picoquic_sack_item_t *) get_pkt_ctx(pkt_ctx, AK_PKTCTX_FIRST_SACK_ITEM);
    if (highest_acknowledged || (uint64_t) get_sack_item(first_sack, AK_SACKITEM_START_RANGE) == (uint64_t)((int64_t)-1)) {
        set_pkt_ctx(pkt_ctx, AK_PKTCTX_HIGHEST_ACKNOWLEDGED, largest);
        is_new_ack = 1;

        if (ack_delay < PICOQUIC_ACK_DELAY_MAX) {
            /* if the ACK is reasonably recent, use it to update the RTT */
            /* find the stored copy of the largest acknowledged packet */
            uint64_t sequence_number = 0;
            if (packet != NULL) {
                sequence_number = get_pkt(packet, AK_PKT_SEQUENCE_NUMBER);
            }

            while (packet != NULL && sequence_number > largest) {
                packet = (picoquic_packet_t *) get_pkt(packet, AK_PKT_NEXT_PACKET);
                if (packet != NULL) {
                    sequence_number = get_pkt(packet, AK_PKT_SEQUENCE_NUMBER);
                }
            }

            if (packet == NULL || sequence_number < largest) {
                /* There is no copy of this packet in store. It may have
                 * been deleted because too old, or maybe already
                 * retransmitted */
            } else {
                uint64_t acknowledged_time = current_time - ack_delay;
                uint64_t send_time = (uint64_t) get_pkt(packet, AK_PKT_SEND_TIME);
                int64_t rtt_estimate = acknowledged_time - send_time;

                uint64_t latest_time_acknowledged = (uint64_t) get_pkt_ctx(pkt_ctx, AK_PKTCTX_LATEST_TIME_ACKNOWLEDGED);
                if (latest_time_acknowledged < send_time) {
                    set_pkt_ctx(pkt_ctx, AK_PKTCTX_LATEST_TIME_ACKNOWLEDGED, send_time);
                }
                set_cnx(cnx, AK_CNX_LATEST_PROGRESS_TIME, 0, current_time);

                if (rtt_estimate > 0) {
                    picoquic_path_t * old_sending_path = (picoquic_path_t *) get_pkt(packet, AK_PKT_SEND_PATH);
                    int old_sending_uniflow_index = mp_get_uniflow_index_from_path(bpfd, true, old_sending_path);
                    uint64_t old_max_ack_delay = (uint64_t) get_path(old_sending_path, AK_PATH_MAX_ACK_DELAY, 0);

                    if (ack_delay > old_max_ack_delay) {
                        set_path(old_sending_path, AK_PATH_MAX_ACK_DELAY, 0, ack_delay);
                    }

                    /* First compute for the tuples */
                    if (receiving_uniflow_index >= 0 && old_sending_uniflow_index >= 0) {
                        stats_t *stats = &bpftd->tuple_stats[receiving_uniflow_index][old_sending_uniflow_index];
                        stats->nb_updates++;
                        if (stats->smoothed_rtt == 0 && stats->rtt_variant == 0) {
                            stats->smoothed_rtt = rtt_estimate;
                            stats->rtt_variant = rtt_estimate / 2;
                            stats->rtt_min = rtt_estimate;
                            stats->max_ack_delay = rtt_estimate / 4;
                            if (stats->max_ack_delay < 1000) {
                                stats->max_ack_delay = 1000;
                            }
                        }
                        else {
                            /* Computation per RFC 6298 */
                            int64_t delta_rtt = rtt_estimate - stats->smoothed_rtt;
                            int64_t delta_rtt_average = 0;
                            stats->smoothed_rtt = (uint64_t) ((int64_t) stats->smoothed_rtt + delta_rtt / 8);

                            if (delta_rtt < 0) {
                                delta_rtt_average = (-delta_rtt) - stats->rtt_variant;
                            } else {
                                delta_rtt_average = delta_rtt - stats->rtt_variant;
                            }
                            stats->rtt_variant += delta_rtt_average / 4;

                            if (rtt_estimate < stats->rtt_min) {
                                stats->rtt_min = rtt_estimate;
                                uint64_t new_ack_delay_local = rtt_estimate / 4;
                                if (new_ack_delay_local < 1000) {
                                    new_ack_delay_local = 1000;
                                } else if (new_ack_delay_local > 10000) {
                                    new_ack_delay_local = 10000;
                                }
                                stats->max_ack_delay = new_ack_delay_local;
                            }

                            if (4 * stats->rtt_variant < stats->rtt_min) {
                                stats->rtt_variant = stats->rtt_min / 4;
                            }
                        }
                    }

                    /* And still keep the old code */
                    uint64_t old_smoothed_rtt = (uint64_t) get_path(old_sending_path, AK_PATH_SMOOTHED_RTT, 0);
                    uint64_t old_rtt_variant = (uint64_t) get_path(old_sending_path, AK_PATH_RTT_VARIANT, 0);
                    if (old_smoothed_rtt == PICOQUIC_INITIAL_RTT && old_rtt_variant == 0) {
                        set_path(old_sending_path, AK_PATH_SMOOTHED_RTT, 0, rtt_estimate);
                        set_path(old_sending_path, AK_PATH_RTT_VARIANT, 0, rtt_estimate / 2);
                        set_path(old_sending_path, AK_PATH_RTT_MIN, 0, rtt_estimate);
                        set_path(old_sending_path, AK_PATH_RETRANSMIT_TIMER, 0, 3 * rtt_estimate + old_max_ack_delay);
                        uint64_t new_ack_delay_local = get_path(old_sending_path, AK_PATH_RTT_MIN, 0) / 4;
                        if (new_ack_delay_local < 1000) {
                            new_ack_delay_local = 1000;
                        }
                        set_pkt_ctx(pkt_ctx, AK_PKTCTX_ACK_DELAY_LOCAL, new_ack_delay_local);
                    } else {
                        /* Computation per RFC 6298 */
                        int64_t delta_rtt = rtt_estimate - old_smoothed_rtt;
                        int64_t delta_rtt_average = 0;
                        set_path(old_sending_path, AK_PATH_SMOOTHED_RTT, 0, old_smoothed_rtt + (delta_rtt) / 8);

                        if (delta_rtt < 0) {
                            delta_rtt_average = (-delta_rtt) - old_rtt_variant;
                        } else {
                            delta_rtt_average = delta_rtt - old_rtt_variant;
                        }
                        set_path(old_sending_path, AK_PATH_RTT_VARIANT, 0, old_rtt_variant + (delta_rtt_average) / 4);

                        uint64_t old_rtt_min = get_path(old_sending_path, AK_PATH_RTT_MIN, 0);
                        if (rtt_estimate < (int64_t)old_rtt_min) {
                            set_path(old_sending_path, AK_PATH_RTT_MIN, 0, rtt_estimate);

                            uint64_t new_ack_delay_local = get_path(old_sending_path, AK_PATH_RTT_MIN, 0) / 4;
                            if (new_ack_delay_local < 1000) {
                                new_ack_delay_local = 1000;
                            } else if (new_ack_delay_local > 10000) {
                                new_ack_delay_local = 10000;
                            }
                            set_pkt_ctx(pkt_ctx, AK_PKTCTX_ACK_DELAY_LOCAL, new_ack_delay_local);
                        }

                        old_rtt_variant = (uint64_t) get_path(old_sending_path, AK_PATH_RTT_VARIANT, 0);
                        old_rtt_min = (uint64_t) get_path(old_sending_path, AK_PATH_RTT_MIN, 0);
                        if (4 * old_rtt_variant < old_rtt_min) {
                            set_path(old_sending_path, AK_PATH_RTT_VARIANT, 0, old_rtt_min / 4);
                        }

                        old_max_ack_delay = (uint64_t) get_path(old_sending_path, AK_PATH_MAX_ACK_DELAY, 0);
                        old_rtt_variant = (uint64_t) get_path(old_sending_path, AK_PATH_RTT_VARIANT, 0);
                        old_smoothed_rtt = (uint64_t) get_path(old_sending_path, AK_PATH_SMOOTHED_RTT, 0);
                        /* FIXME adapt the retransmit timer computation to the asymmetric paths */
                        set_path(old_sending_path, AK_PATH_RETRANSMIT_TIMER, 0, old_smoothed_rtt + 4 * old_rtt_variant + old_max_ack_delay);
                    }
                    set_path(old_sending_path, AK_PATH_RTT_SAMPLE, 0, rtt_estimate);

                    uint64_t old_retransmit_timer = (uint64_t) get_path(old_sending_path, AK_PATH_RETRANSMIT_TIMER, 0);
                    if (PICOQUIC_MIN_RETRANSMIT_TIMER > old_retransmit_timer) {
                        set_path(old_sending_path, AK_PATH_RETRANSMIT_TIMER, 0, PICOQUIC_MIN_RETRANSMIT_TIMER);
                    }

                    picoquic_congestion_algorithm_t *congestion_alg = (picoquic_congestion_algorithm_t *) get_cnx(cnx, AK_CNX_CONGESTION_CONTROL_ALGORITHM, 0);
                    if (congestion_alg != NULL) {
                        helper_congestion_algorithm_notify(cnx, old_sending_path,
                            picoquic_congestion_notification_rtt_measurement,
                            rtt_estimate, 0, 0, current_time);
                    }
                }
            }
        }
    }

    set_cnx(cnx, AK_CNX_OUTPUT, 0, (protoop_arg_t) is_new_ack);
    return (protoop_arg_t) packet;
}