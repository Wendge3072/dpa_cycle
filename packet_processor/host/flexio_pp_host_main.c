#include "flexio_pp_host_utils.h"

size_t scheduler_num = 1;
size_t tenants_num = 2;
size_t threads_num_per_scheduler = 8;
size_t threads_num = 0;
size_t begin_thread = 16;
uint64_t DMAC = 0xa088c2320440;
size_t buffer_location = 0;
size_t use_copy = 1;

// #define nic_mode 1
/* Main host side function.
 * Responsible for allocating resources and making preparations for DPA side envocatin.
 */
int main(int argc, char **argv)
{
	if (argc > 2) {
		scheduler_num = atoi(argv[2]);
	}
	printf("scheduler_num: %d\n", scheduler_num);

    if (argc > 3) {
        tenants_num = atoi(argv[3]);
    }

	if (argc > 4) {
		threads_num_per_scheduler = atoi(argv[4]);
	}

	threads_num = threads_num_per_scheduler * scheduler_num;

	if (argc > 5) {
        begin_thread = atoi(argv[5]);
		if (begin_thread < ((scheduler_num + 15) / 16) * 16) {
			printf("Invalid begin_thread value. It must be at least %d.\n", ((scheduler_num + 15) / 16) * 16);
			return -1;
		}
    }

	if (argc > 6) {
		buffer_location = atoi(argv[6]);
	}

	char buf[2];
	int err = 0;
	flexio_status ret = 0;
	struct flexio_process_attr process_attr = { NULL, 0 };
	struct app_context app_ctx = {};
	struct thread_context* thd_ctx = NULL;
	struct thread_context* sch_ctx = NULL;

	printf("Welcome to Flex IO SDK packet processing app.\n");

	thd_ctx = malloc(sizeof(struct thread_context) * threads_num);
	if (thd_ctx == NULL) {
		printf("malloc thread context failed\n");
		return -1;
	}
	for (int i = 0; i < threads_num; i++) {
		thd_ctx[i].queues = malloc(sizeof(struct flexio_queues));
		if (thd_ctx[i].queues == NULL) {
			printf("malloc queue context failed\n");
			return -1;
		}
		thd_ctx[i].num_queues = 1;
	}

	sch_ctx = malloc(sizeof(struct thread_context) * scheduler_num);
	if (sch_ctx == NULL) {
		printf("malloc scheduler context failed\n");
		return -1;
	}
	for (int i = 0; i < scheduler_num; i++) {
		sch_ctx[i].queues = malloc(sizeof(struct flexio_queues) * threads_num_per_scheduler);
		if (sch_ctx[i].queues == NULL) {
			printf("malloc scheduler queue context failed\n");
			return -1;
		}
		sch_ctx[i].num_queues = threads_num_per_scheduler;
	}

	if (geteuid()) {
		printf("Failed - the application must run with root privileges\n");
		return -1;
	}

	err = app_open_ibv_ctx(&(app_ctx), argv[1]);
	if (err) {
		printf("Failed to open ibv context.\n");
		return -1;
		goto cleanup;
	}

	app_ctx.process_pd = ibv_alloc_pd(app_ctx.ibv_ctx);
	if (app_ctx.process_pd == NULL) {
		printf("Failed to create pd.\n");
		err = -1;
		goto cleanup;
	}

	if (flexio_process_create(app_ctx.ibv_ctx, DEV_APP_NAME, &process_attr, &(app_ctx.flexio_process))) {
		printf("Failed to create Flex IO process.\n");
		err = -1;
		goto cleanup;
	}

	ret = flexio_window_create(app_ctx.flexio_process, app_ctx.process_pd, &(app_ctx.flexio_window));
	if (ret != FLEXIO_STATUS_SUCCESS) {
		printf("Failed to create FlexIO window\n");
		err = -1;
		goto cleanup;
	}

	app_ctx.process_uar = flexio_process_get_uar(app_ctx.flexio_process);

	flexio_msg_stream_attr_t stream_fattr = {0};
	stream_fattr.uar = app_ctx.process_uar;
	stream_fattr.data_bsize = 4 * 2048;
	stream_fattr.sync_mode = FLEXIO_LOG_DEV_SYNC_MODE_SYNC;
	stream_fattr.level = FLEXIO_MSG_DEV_DEBUG;
	stream_fattr.stream_name = "Default Stream";
	stream_fattr.mgmt_affinity.type = FLEXIO_AFFINITY_NONE;
	if (flexio_msg_stream_create(app_ctx.flexio_process, &stream_fattr, stdout, NULL,
						&(app_ctx.stream))) {
		printf("Failed to init device messaging environment, exiting App\n");
		err = -1;
		goto cleanup;
	}

	app_ctx.rx_matcher = create_matcher_rx(app_ctx.ibv_ctx);
#ifndef nic_mode
	app_ctx.tx_matcher = create_matcher_tx(app_ctx.ibv_ctx);
#endif


#define THREAD_NUM 190
#define THREAD_RUNNING_BITMAP 32
	/* Create thread running bitmap on the DPA heap */
	// flexio_buf_dev_alloc(app_ctx.flexio_process, THREAD_RUNNING_BITMAP, &app_ctx.dpa_thread_running_bm_daddr);

	for (int i = 0; i < scheduler_num; i++) {
		struct flexio_event_handler_attr handler_attr = {0};
		handler_attr.host_stub_func = flexio_scheduler_handle;
		handler_attr.affinity.type = FLEXIO_AFFINITY_STRICT;
		handler_attr.affinity.id = 0 + i;

		ret = flexio_event_handler_create(app_ctx.flexio_process, &handler_attr, &(sch_ctx[i].event_handler));
		if (ret != FLEXIO_STATUS_SUCCESS) {
			printf("Fail tp create event handler.\n");
			goto cleanup;
		}
		void* tmp_ptr = NULL;
		size_t needed_buffer_size = SPEED_RESULT_SIZE;
		size_t mmap_size = needed_buffer_size + (64 - 1);
		mmap_size -= mmap_size % 64;
		tmp_ptr = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
		if (tmp_ptr == NULL) {
			printf("Failed to allocate host buffer\n");
			return -1;
		}
		memset(tmp_ptr, 0, mmap_size);
		sch_ctx[i].mr = ibv_reg_mr(app_ctx.process_pd, tmp_ptr, mmap_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
		if (sch_ctx[i].mr == NULL) {
			printf("Failed to register MR\n");
			return -1;
		}
		sch_ctx[i].result_buffer_mkey_id = sch_ctx[i].mr->lkey;
		sch_ctx[i].result_buffer = (char*)tmp_ptr;
		sch_ctx[i].thd_id = i;
		if (create_app_rq(&(app_ctx), &(sch_ctx[i]))) {
			printf("Failed to create Flex EQ.\n");
			err = -1;
			goto cleanup;
		}
		if (create_app_sq(&(app_ctx), &(sch_ctx[i]), use_copy)) {
			printf("Failed to create Flex SQ.\n");
			err = -1;
			goto cleanup;
		}


		for (int j = 0; j < threads_num_per_scheduler; j++) {
			uint64_t cur_dmac = DMAC + i * threads_num_per_scheduler + j;
			// uint64_t cur_dmac = DMAC + (uint64_t)(j * 2 + (i & 1));

			sch_ctx[i].queues[j].rq_tir_obj = flexio_rq_get_tir(sch_ctx[i].queues[j].flexio_rq_ptr);
			if (sch_ctx[i].queues[j].rq_tir_obj == NULL) {
				printf("Fail creating rq_tir_obj (errno %d)\n", errno);
				goto cleanup;
			}
			sch_ctx[i].queues[j].rx_flow_rule = create_rule_rx_mac_match(app_ctx.rx_matcher, sch_ctx[i].queues[j].rq_tir_obj, cur_dmac);
			sch_ctx[i].queues[j].tx_flow_rule = create_rule_tx_fwd_to_sws_table(app_ctx.tx_matcher, cur_dmac);
			sch_ctx[i].queues[j].tx_flow_rule2 = create_rule_tx_fwd_to_vport(app_ctx.tx_matcher, cur_dmac);
		}

		if (copy_sch_data_to_dpa(&app_ctx, &(sch_ctx[i]), buffer_location, use_copy)) {
			printf("Failed to copy application data to DPA.\n");
			err = -1;
			goto cleanup;
		}

		if (flexio_event_handler_run(sch_ctx[i].event_handler, sch_ctx[i].app_data_daddr)) {
			printf("Failed to run event handler.\n");
			err = -1;
			goto cleanup;
		}
	}

	for (int i = 0; i < threads_num; i++) {
		struct flexio_event_handler_attr handler_attr = {0};
		uint64_t rpc_ret_val = 0;

		if(i % 2)
        	handler_attr.host_stub_func = flexio_pp_dev_31;
		else
			handler_attr.host_stub_func = flexio_pp_dev_32;

        handler_attr.affinity.type = FLEXIO_AFFINITY_STRICT;
		handler_attr.affinity.id = i + begin_thread;

        ret = flexio_event_handler_create(app_ctx.flexio_process, &handler_attr, &(thd_ctx[i].event_handler));
        if (ret != FLEXIO_STATUS_SUCCESS) {
			printf("Fail tp create event handler.\n");
			goto cleanup;
		}
		void* tmp_ptr = NULL;
		size_t needed_buffer_size = SPEED_RESULT_SIZE + NVME_QUEUE_MEMORY_SIZE;
		size_t mmap_size = needed_buffer_size + (64 - 1);
		mmap_size -= mmap_size % 64;
		tmp_ptr = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
		if (tmp_ptr == NULL) {
			printf("Failed to allocate host buffer\n");
			return -1;
		}
		memset(tmp_ptr, 0, mmap_size);
		thd_ctx[i].mr = ibv_reg_mr(app_ctx.process_pd, tmp_ptr, mmap_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
		if (thd_ctx[i].mr == NULL) {
			printf("Failed to register MR\n");
			return -1;
		}
		thd_ctx[i].result_buffer_mkey_id = thd_ctx[i].mr->lkey;
		thd_ctx[i].result_buffer = (char*)tmp_ptr;
		thd_ctx[i].host_buffer = (char*)tmp_ptr + SPEED_RESULT_SIZE;
		thd_ctx[i].thd_id = i;

		if (create_app_rq(&(app_ctx), &(thd_ctx[i]))) {
			printf("Failed to create Flex EQ.\n");
			err = -1;
			goto cleanup;
		}

		if (create_app_sq(&(app_ctx), &(thd_ctx[i]), use_copy)) {
			printf("Failed to create Flex SQ.\n");
			err = -1;
			goto cleanup;
		}

		if (copy_thd_data_to_dpa(&app_ctx, &(thd_ctx[i]), buffer_location, use_copy)) {
			printf("Failed to copy application data to DPA.\n");
			err = -1;
			goto cleanup;
		}
		
		flexio_process_call(app_ctx.flexio_process, &thd_ctx_init, &rpc_ret_val, thd_ctx[i].app_data_daddr);

		if (flexio_event_handler_run(thd_ctx[i].event_handler, thd_ctx[i].app_data_daddr)) {
			printf("Failed to run event handler.\n");
			err = -1;
			goto cleanup;
		}

	}

	/* Wait for Enter - the DPA sample is running in the meanwhile */
	if (!fread(buf, 1, 1, stdin)) {
		printf("Failed in fread\n");
	}

cleanup:
	/* Clean up flow is done in reverse order of creation as there's a refernce system
	 * that won't allow destroying resources that has references to existing resources.
	 */

	for (size_t i = 0; i < threads_num; i++) {    
        if (thd_ctx[i].app_data_daddr && flexio_buf_dev_free(app_ctx.flexio_process, thd_ctx[i].app_data_daddr)) {
    	    printf("Failed to dealloc application data memory on Flex IO heap\n");
        }
    }
	for (size_t i = 0; i < scheduler_num; i++) {    
		if (sch_ctx[i].app_data_daddr && flexio_buf_dev_free(app_ctx.flexio_process, sch_ctx[i].app_data_daddr)) {
		    printf("Failed to dealloc application data memory on Flex IO heap\n");
		}
	}

	for (size_t i = 0; i < scheduler_num; i++) { 
		for (size_t j = 0; j < threads_num_per_scheduler; j++) { 
			/* Clean up rx rule if created */
			if (sch_ctx[i].queues[j].rx_flow_rule) {
				if (destroy_rule(sch_ctx[i].queues[j].rx_flow_rule)) {
					printf("Failed to destroy rx rule\n");
				}
			}
			if (sch_ctx[i].queues[j].tx_flow_rule) {
				if (destroy_rule(sch_ctx[i].queues[j].tx_flow_rule)) {
					printf("Failed to destroy tx rule\n");
				}
			}
			if (sch_ctx[i].queues[j].tx_flow_rule2) {
				if (destroy_rule(sch_ctx[i].queues[j].tx_flow_rule2)) {
					printf("Failed to destroy tx rule2\n");
				}
			}
		}
	}

	for (size_t i = 0; i < threads_num; i++) { 
        /* Clean up rx rule if created */
        if (thd_ctx[i].queues->rx_flow_rule) {
            if (destroy_rule(thd_ctx[i].queues->rx_flow_rule)) {
                printf("Failed to destroy rx rule\n");
            }
        }
		if (thd_ctx[i].queues->tx_flow_rule) {
            if (destroy_rule(thd_ctx[i].queues->tx_flow_rule)) {
                printf("Failed to destroy tx rule\n");
            }
        }
		if (thd_ctx[i].queues->tx_flow_rule2) {
            if (destroy_rule(thd_ctx[i].queues->tx_flow_rule2)) {
                printf("Failed to destroy tx rule2\n");
            }
        }
    }

    if (app_ctx.rx_matcher && destroy_matcher(app_ctx.rx_matcher)) {
        printf("Failed to destroy rx matcher\n");
    }

    if (app_ctx.rx_matcher && destroy_matcher(app_ctx.rx_matcher)) {
        printf("Failed to destroy rx matcher\n");
    }

	for (size_t i = 0; i < threads_num; i++) {
		/* Clean up previously allocated SQ */
		if (clean_up_app_sq(&app_ctx, &(thd_ctx[i]))) {
            printf("Failed to destroy sq\n");
		}

		/* Clean up previously allocated RQ */
		if (clean_up_app_rq(&app_ctx, &(thd_ctx[i]))) {
            printf("Failed to destroy cq\n");
		}
		if (thd_ctx[i].event_handler && flexio_event_handler_destroy(thd_ctx[i].event_handler)) {
            printf("Failed to destroy event handler\n");
		}
	}

	for (size_t i = 0; i < scheduler_num; i++) {
		/* Clean up previously allocated SQ */
		if (clean_up_app_sq(&app_ctx, &(sch_ctx[i]))) {
			printf("Failed to destroy sq\n");
		}

		/* Clean up previously allocated RQ */
		if (clean_up_app_rq(&app_ctx, &(sch_ctx[i]))) {
			printf("Failed to destroy cq\n");
		}
		if (sch_ctx[i].event_handler && flexio_event_handler_destroy(sch_ctx[i].event_handler)) {
			printf("Failed to destroy event handler\n");
		}
	}

	if (app_ctx.stream && flexio_msg_stream_destroy(app_ctx.stream)) {
		printf("Failed to destroy device messaging environment\n");
	}

	if (app_ctx.flexio_window && flexio_window_destroy(app_ctx.flexio_window)) {
		printf("Failed to destroy window.\n");
	}

	if (app_ctx.flexio_process && flexio_process_destroy(app_ctx.flexio_process)) {
		printf("Failed to destroy process.\n");
	}

	/* Close the IBV device */
	if (app_ctx.ibv_ctx && ibv_close_device(app_ctx.ibv_ctx)) {
		printf("Failed to close ibv context.\n");
	}

	return err;
}
