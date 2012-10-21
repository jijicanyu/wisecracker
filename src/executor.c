/*
    Wisecracker: A cryptanalysis framework
    Copyright (c) 2011-2012, Vikas Naresh Kumar, Selective Intellect LLC
       
   	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    any later version.
   
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
   
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
 * Copyright: 2011. Selective Intellect LLC. All Rights Reserved.
 * Author: Vikas Kumar
 * Date: 16th Oct 2012
 * Software: WiseCracker
 */
#include <wisecracker.h>

#include "internal_mpi.h"
#include "internal_opencl.h"

enum {
	WC_EXECSTATE_NOT_STARTED = 0,
	WC_EXECSTATE_OPENCL_INITED,
	WC_EXECSTATE_STARTED,
	WC_EXECSTATE_GOT_CODE,
	WC_EXECSTATE_GOT_BUILDOPTS,
	WC_EXECSTATE_COMPILED_CODE,
	WC_EXECSTATE_GOT_NUMTASKS,
	WC_EXECSTATE_GOT_TASKRANGEMULTIPLIER,
	WC_EXECSTATE_GOT_TASKSPERSYSTEM,
	WC_EXECSTATE_GOT_GLOBALDATA,
	WC_EXECSTATE_DEVICE_STARTED,
	WC_EXECSTATE_DEVICE_DONE_RUNNING,
	WC_EXECSTATE_DEVICE_FINISHED,
	WC_EXECSTATE_FREED_GLOBALDATA,
	WC_EXECSTATE_FINISHED
};

struct wc_executor_details {
	int num_systems;
	int system_id;
	uint8_t mpi_initialized;
	wc_exec_callbacks_t cbs;
	uint8_t callbacks_set;
	wc_opencl_t ocl;
	uint8_t ocl_initialized;
	char *code;
	size_t codelen;
	char *buildopts;
	uint64_t num_tasks;
	uint32_t task_range_multiplier;
	wc_data_t globaldata;
	int state;
	// we track each system's total possibilities
	uint64_t *tasks_per_system;
	int64_t refcount;
	cl_event userevent;
};

wc_exec_t *wc_executor_init(int *argc, char ***argv)
{
	int rc = 0;
	wc_exec_t *wc = NULL;

	do {
		wc = WC_MALLOC(sizeof(wc_exec_t));
		if (!wc) {
			WC_ERROR_OUTOFMEMORY(sizeof(wc_exec_t));
			wc = NULL;
			break;
		}
		// this memset is necessary
		memset(wc, 0, sizeof(wc_exec_t));
		rc = wc_mpi_init(argc, argv);
		if (rc != 0)
			break;
		wc->mpi_initialized = 1;
		wc->num_systems = wc_mpi_peer_count();
		if (wc->num_systems < 0) {
			rc = -1;
			break;
		}
		wc->system_id = wc_mpi_peer_id();
		if (wc->system_id < 0) {
			rc = -1;
			break;
		}
		wc->ocl_initialized = 0;
		wc->callbacks_set = 0;
		wc->cbs.user = NULL;
		wc->cbs.max_devices = 0;
		wc->cbs.device_type = WC_DEVTYPE_ANY;
		wc->state = WC_EXECSTATE_NOT_STARTED;
	} while (0);
	if (rc < 0) {
		if (wc && wc->mpi_initialized) {
			wc_mpi_abort(-1);
			wc->mpi_initialized = 0;
		}
		WC_FREE(wc);
	}
	return wc;
}

void wc_executor_destroy(wc_exec_t *wc)
{
	if (wc) {
		int rc = 0;
		if (wc->ocl_initialized) {
			wc_opencl_finalize(&wc->ocl);
			wc->ocl_initialized = 0;
		}
		WC_FREE(wc->tasks_per_system);
		WC_FREE(wc->globaldata.ptr);
		wc->globaldata.len = 0;
		WC_FREE(wc->code);
		WC_FREE(wc->buildopts);
		wc->codelen = 0;
		wc->num_tasks = 0;
		if (wc->mpi_initialized) {
			rc = wc_mpi_finalize();
			if (rc != 0) {
				WC_WARN("MPI Finalize error.\n");
			}
			wc->mpi_initialized = 0;
		}
		memset(wc, 0, sizeof(*wc));
		WC_FREE(wc);
	}
}

int wc_executor_num_systems(const wc_exec_t *wc)
{
	return (wc) ? wc->num_systems : WC_EXE_ERR_INVALID_PARAMETER;
}

int wc_executor_system_id(const wc_exec_t *wc)
{
	return (wc) ? wc->system_id : WC_EXE_ERR_INVALID_PARAMETER;
}

wc_err_t wc_executor_setup(wc_exec_t *wc, const wc_exec_callbacks_t *cbs)
{
	if (wc && cbs) {
		// verify if the required callbacks are present
		if (!cbs->get_code || !cbs->get_num_tasks ||
			!cbs->on_device_range_exec) {
			WC_ERROR("Wisecracker needs the get_code, get_num_tasks and"
					" on_device_range_exec callbacks.\n");
			wc->callbacks_set = 0;
		} else {
			uint32_t data[2];
			wc_devtype_t devtype;
			uint32_t maxdevs = 0;
			data[0] = (uint32_t)cbs->device_type;
			data[1] = cbs->max_devices;
			if (wc->mpi_initialized) {
				int rc = wc_mpi_broadcast(data, 2, MPI_INT, 0);
				if (rc < 0) {
					WC_ERROR("Unable to share the device type and max devices."
							" MPI Error: %d\n", rc);
					return WC_EXE_ERR_MPI;
				}
			}
			devtype = (wc_devtype_t)data[0];
			maxdevs = data[1];
			// ok we were initialized. let's check if the device-type is same
			// and max-devices is same too
			if (wc->ocl_initialized) {
				if (wc->cbs.device_type == devtype &&
					wc->cbs.max_devices == maxdevs) {
					// do nothing
				} else {
					// finalize it to be reinitialized with a different set of
					// devices and device types
					wc_opencl_finalize(&wc->ocl);
					wc->ocl_initialized = 0;
					WC_DEBUG("Finalizing OpenCL to reinitialize again since"
							" device count and type are changing\n");
				}
			}
			if (!wc->ocl_initialized) {
				if (wc_opencl_init(devtype, maxdevs, &wc->ocl, 0) < 0) {
					WC_ERROR("Failed to create local runtime on system\n");
					return WC_EXE_ERR_OPENCL;
				}
			}
			wc->ocl_initialized = 1;
			// copy the pointers into an internal structure
			memcpy(&(wc->cbs), cbs, sizeof(*cbs));
			// override the values with the received values
			wc->cbs.device_type = devtype;
			wc->cbs.max_devices = maxdevs;
			wc->callbacks_set = 1;
			return WC_EXE_OK;
		}
	}
	return WC_EXE_ERR_INVALID_PARAMETER;
}

static wc_err_t wc_executor_pre_run(wc_exec_t *wc)
{
	wc_err_t rc = WC_EXE_OK;
	if (!wc)
		return WC_EXE_ERR_INVALID_PARAMETER;
	do {
		wc->state = WC_EXECSTATE_NOT_STARTED;
		if (!wc->callbacks_set) {
			WC_ERROR("Callbacks not set for executor.\n");
			rc = WC_EXE_ERR_MISSING_CALLBACK;
			break;
		}
		if (!wc->ocl_initialized) {
			if (wc_opencl_init(wc->cbs.device_type, wc->cbs.max_devices,
						&wc->ocl, 0) < 0) {
				WC_ERROR("Failed to create local runtime on system\n");
				rc = WC_EXE_ERR_OPENCL;
				break;
			}
			wc->ocl_initialized = 1;
		}
		wc->state = WC_EXECSTATE_OPENCL_INITED;
		if (!wc_opencl_is_usable(&wc->ocl)) {
			WC_ERROR("OpenCL internal runtime is not usable\n");
			rc = WC_EXE_ERR_BAD_STATE;
			break;
		}
		// call the on_start event
		if (wc->cbs.on_start) {
			rc = wc->cbs.on_start(wc, wc->cbs.user);
			if (rc != WC_EXE_OK) {
				WC_ERROR("Error in on_start callback: %d\n", rc);
				break;
			}
		}
		wc->state = WC_EXECSTATE_STARTED;
		// call the get_code function to retrieve the code
		if (!wc->cbs.get_code) {
			WC_ERROR("The get_code callback is missing\n");
			rc = WC_EXE_ERR_MISSING_CALLBACK;
			break;
		} else {
			wc->codelen = 0;
			// clear the code previously loaded
			WC_FREE(wc->code);
			wc->code = wc->cbs.get_code(wc, wc->cbs.user, &wc->codelen);
			if (!wc->code || wc->codelen == 0) {
				WC_ERROR("Error in get_code callback: %d\n", rc);
				break;	
			}
		}
		wc->state = WC_EXECSTATE_GOT_CODE;
		// call the build_opts function to retrieve the build options
		if (wc->cbs.get_build_options) {
			WC_FREE(wc->buildopts); // clear the previous loaded options
			wc->buildopts = wc->cbs.get_build_options(wc, wc->cbs.user);
			if (!wc->buildopts) {
				WC_WARN("Build options returned was NULL.\n");
			}
		}
		wc->state = WC_EXECSTATE_GOT_BUILDOPTS;
		// ok compile the code now
		if (wc_opencl_program_load(&wc->ocl, wc->code, wc->codelen,
									wc->buildopts) < 0) {
			WC_ERROR("Unable to compile OpenCL code.\n");
			rc = WC_EXE_ERR_OPENCL;
			if (wc->cbs.on_code_compile)
				wc->cbs.on_code_compile(wc, wc->cbs.user, 0);
			break;
		}
		if (wc->cbs.on_code_compile)
			wc->cbs.on_code_compile(wc, wc->cbs.user, 1);
		wc->state = WC_EXECSTATE_COMPILED_CODE;
	} while (0);
	return rc;
}

static wc_err_t wc_executor_post_run(wc_exec_t *wc)
{
	wc_err_t rc = WC_EXE_OK;
	if (!wc)
		return WC_EXE_ERR_INVALID_PARAMETER;
	if (wc->cbs.free_global_data) {
		wc->cbs.free_global_data(wc, wc->cbs.user, &wc->globaldata);
	} else {
		// free the global data
		WC_FREE(wc->globaldata.ptr);
	}
	wc->globaldata.ptr = NULL;
	wc->globaldata.len = 0;
	wc->state = WC_EXECSTATE_FREED_GLOBALDATA;
	// call the on_finish event
	if (wc->state != WC_EXECSTATE_NOT_STARTED && wc->cbs.on_finish) {
		rc = wc->cbs.on_finish(wc, wc->cbs.user);
		if (rc != WC_EXE_OK) {
			WC_ERROR("Error in on_finish callback: %d\n", rc);
		}
		wc->state = WC_EXECSTATE_FINISHED;
	}
	return rc;
}

static uint64_t wc_executor_tasks_per_system(const wc_opencl_t *ocl)
{
	uint64_t total_tasks = 0;
	if (ocl) {
		uint32_t idx;
		for (idx = 0; idx < ocl->device_max; ++idx) {
			const wc_cldev_t *dev = &ocl->devices[idx];
			total_tasks += (dev->workgroup_sz * dev->compute_units);
		}
	}
	return total_tasks;
}

static wc_err_t wc_executor_master_run(wc_exec_t *wc)
{
	wc_err_t rc = WC_EXE_OK;
	if (!wc)
		return WC_EXE_ERR_INVALID_PARAMETER;
	do {
		// get task decomposition
		// TODO: we can add another callback to retrieve a more detailed
		// decomposition, but not yet
		if (!wc->cbs.get_num_tasks) {
			WC_ERROR("The get_num_tasks callback is missing\n");
			rc = WC_EXE_ERR_MISSING_CALLBACK;
			break;
		} else {
			wc->num_tasks = wc->cbs.get_num_tasks(wc, wc->cbs.user);
			if (wc->num_tasks == 0) {
				WC_ERROR("Task size cannot be 0.\n");
				rc = WC_EXE_ERR_INVALID_VALUE;
				break;
			}
			WC_DEBUG("No of Tasks: %"PRIu64"\n", wc->num_tasks);
		}
		wc->state = WC_EXECSTATE_GOT_NUMTASKS;
		if (wc->cbs.get_task_range_multiplier) {
			wc->task_range_multiplier = wc->cbs.get_task_range_multiplier(wc,
										wc->cbs.user);
		}
		if (wc->task_range_multiplier < 1)
			wc->task_range_multiplier = 1;
		wc->state = WC_EXECSTATE_GOT_TASKRANGEMULTIPLIER;
		//TODO: exchange the wc_runtime_t device information across the systems
		// receive the device information from other systems

		if (!wc->ocl_initialized) {
			WC_ERROR("OpenCL is not initialized.\n");
			rc = WC_EXE_ERR_BAD_STATE;
			break;
		}
		wc->tasks_per_system = WC_MALLOC(wc->num_systems * sizeof(uint64_t));
		if (!wc->tasks_per_system) {
			WC_ERROR_OUTOFMEMORY(wc->num_systems * sizeof(uint64_t));
			rc = WC_EXE_ERR_OUTOFMEMORY;
			break;
		}
		wc->tasks_per_system[wc->system_id] =
						wc_executor_tasks_per_system(&wc->ocl);
		wc->state = WC_EXECSTATE_GOT_TASKSPERSYSTEM;

		if (wc->cbs.get_global_data) {
			wc_data_t gdata = { 0 };
			rc = wc->cbs.get_global_data(wc, wc->cbs.user, &gdata);
			if (rc != WC_EXE_OK) {
				WC_ERROR("Error retrieving global data: %d\n", rc);
				break;
			}
			wc->globaldata.ptr = gdata.ptr;
			wc->globaldata.len = gdata.len;
		}
		wc->state = WC_EXECSTATE_GOT_GLOBALDATA;
		//TODO: send the global data across
		// TODO: Do data decomposition here
	} while (0);
	return rc;
}

static wc_err_t wc_executor_slave_run(wc_exec_t *wc)
{
	// TODO: receive the global data here
	// send device information from other systems
	//uint64_t tasks_per_system = wc_executor_tasks_per_system(&wc->ocl);
	return WC_EXE_OK;
}

void CL_CALLBACK wc_executor_device_event_notify(cl_event ev, cl_int status, void *user)
{
	cl_int rc = CL_SUCCESS;
	wc_exec_t *wc = (wc_exec_t *)user;
	if (!wc || !wc->userevent)
		return;
	// reduce the reference count until it hits 1
	wc->refcount--;
	// WC_DEBUG("wrefcount=%ld \n", wc->refcount);
	// the reference count has hit 0
	if (wc->refcount == 0) {
		// WC_DEBUG("setting the user event\n");
		rc = clSetUserEventStatus(wc->userevent, CL_COMPLETE);
		if (rc != CL_SUCCESS)
			WC_ERROR_OPENCL(clSetUserEventStatus, rc);
	}
}

wc_err_t wc_executor_run(wc_exec_t *wc, long timeout)
{
	wc_err_t rc = WC_EXE_OK;
	if (!wc)
		return WC_EXE_ERR_INVALID_PARAMETER;
	do {
		cl_event *events = NULL;
		cl_ulong2 *device_ranges = NULL;
		wc->state = WC_EXECSTATE_NOT_STARTED;
		rc = wc_executor_pre_run(wc);
		if (rc != WC_EXE_OK)
			break;
		if (wc->system_id == 0) {
			rc = wc_executor_master_run(wc);
		} else {
			rc = wc_executor_slave_run(wc);
		}
		if (rc != WC_EXE_OK)
			break;
		if (!wc_opencl_is_usable(&wc->ocl)) {
			WC_ERROR("OpenCL internal runtime is not usable\n");
			rc = WC_EXE_ERR_BAD_STATE;
			break;
		}
		// ok let's invoke the device start functions
		if (wc->cbs.on_device_start) {
			uint32_t idx;
			for (idx = 0; idx < wc->ocl.device_max; ++idx) {
				wc_cldev_t *dev = &(wc->ocl.devices[idx]);
				rc = wc->cbs.on_device_start(wc, dev, idx, wc->cbs.user,
											&wc->globaldata);
				if (rc != WC_EXE_OK) {
					WC_ERROR("Device %u returned error: %d\n", idx, rc);
					break;
				}
			}
			if (rc != WC_EXE_OK)
				break;
		}
		wc->state = WC_EXECSTATE_DEVICE_STARTED;
		events = WC_MALLOC(sizeof(cl_event) * wc->ocl.device_max);
		if (!events) {
			WC_ERROR_OUTOFMEMORY(sizeof(cl_event) * wc->ocl.device_max);
			rc = WC_EXE_ERR_OUTOFMEMORY;
			break;
		}
		memset(events, 0, sizeof(cl_event) * wc->ocl.device_max);
		device_ranges = WC_MALLOC(sizeof(cl_ulong2) * wc->ocl.device_max);
		if (!device_ranges) {
			WC_ERROR_OUTOFMEMORY(sizeof(cl_ulong2) * wc->ocl.device_max);
			rc = WC_EXE_ERR_OUTOFMEMORY;
			break;
		}
		memset(device_ranges, 0, sizeof(cl_ulong2) * wc->ocl.device_max);
		// TODO: we need to do the main stuff here for the current system
		do {
			uint64_t tasks_per_system;
			uint64_t start, end;
			int sid = wc->system_id;
			uint64_t tasks_completed = 0;
			uint32_t idx;

			tasks_per_system = wc->tasks_per_system[sid] *
								wc->task_range_multiplier;
			tasks_completed = 0;
			start = tasks_completed;
			do {
				rc = WC_EXE_OK;
				wc->refcount = 0;
				wc->userevent = (cl_event)0;
				for (idx = 0; idx < wc->ocl.device_max; ++idx) {
					if (wc->ocl.devices[idx].context) {
						wc->userevent =
							clCreateUserEvent(wc->ocl.devices[idx].context,
									&rc);
						if (rc != CL_SUCCESS) {
							WC_ERROR_OPENCL(clRetainEvent, rc);
							// try again
						} else {
							break;
						}
					}
				}
				if (!wc->userevent) {
					rc = WC_EXE_ERR_OPENCL;
					WC_WARN("User event failed to set. Shaky state\n");
					break;
				}
				for (idx = 0; idx < wc->ocl.device_max; ++idx) {
					wc_cldev_t *dev = &(wc->ocl.devices[idx]);
					events[idx] = (cl_event)0;
					end = start + tasks_per_system;
					if (end >= wc->num_tasks)
						end = wc->num_tasks;
					device_ranges[idx].s[0] = start;
					device_ranges[idx].s[1] = end;
					rc = wc->cbs.on_device_range_exec(wc, dev, idx,
							wc->cbs.user, &wc->globaldata, start, end,
							&events[idx]);
					if (rc != WC_EXE_OK) {
						WC_ERROR("Error occurred while running device work:"
								" Range(%"PRIu64",%"PRIu64"). Completed(%"PRIu64")\n",
								start, end, tasks_completed);
						break;
					}
					wc->refcount++;
					//Wait for events here
					if (events[idx]) {
						rc = clSetEventCallback(events[idx], CL_COMPLETE,
								wc_executor_device_event_notify, wc);
						WC_ERROR_OPENCL_BREAK(clSetEventCallback, rc);
						rc = clEnqueueWaitForEvents(dev->cmdq, 1, &events[idx]);
						WC_ERROR_OPENCL_BREAK(clEnqueueWaitForEvents, rc);
					}
					// flush the command queue for the device
					rc = clFlush(dev->cmdq);
					WC_ERROR_OPENCL_BREAK(clFlush, rc);
					tasks_completed += end - start;
					start = end;
					if (start >= wc->num_tasks)
						break;
					if (tasks_completed >= wc->num_tasks)
						break;
				}
				if (rc != WC_EXE_OK || rc < 0)
					break;
				rc = clWaitForEvents(1, &wc->userevent);
				WC_ERROR_OPENCL_BREAK(clWaitForEvents, rc);
				// gets here when event is set
				rc |= clReleaseEvent(wc->userevent);
				wc->userevent = (cl_event)0;
				for (idx = 0; idx < wc->ocl.device_max; ++idx) {
					if (events[idx])
						rc |= clReleaseEvent(events[idx]);
					events[idx] = (cl_event)0;
				}
				if (wc->cbs.on_device_range_done) {
					for (idx = 0; idx < wc->ocl.device_max; ++idx) {
						wc_cldev_t *dev = &(wc->ocl.devices[idx]);
						wc_err_t err = wc->cbs.on_device_range_done(wc, dev,
								idx, wc->cbs.user, &wc->globaldata,
								device_ranges[idx].s[0],
								device_ranges[idx].s[1]);
						//TODO: send data back to master
						if (err == WC_EXE_ABORT) {
							WC_INFO("User requested abort.\n");
							rc = err;
							break;
						}
						if (err != WC_EXE_OK) {
							WC_ERROR("Error occurred in callback\n");
							rc = WC_EXE_ERR_BAD_STATE;
							break;
						}
					}
				}
				if (wc->cbs.progress) {
					double percent = ((double)(100.0 * tasks_completed)) /
															wc->num_tasks;
					wc->cbs.progress((float)percent, wc->cbs.user);
				}
				if (rc != WC_EXE_OK)
					break;
			} while (tasks_completed < wc->num_tasks);
			if (rc != WC_EXE_OK)
				break;
		} while (0);
		wc->state = WC_EXECSTATE_DEVICE_DONE_RUNNING;
		WC_FREE(events);
		WC_FREE(device_ranges);
		if (rc != WC_EXE_OK && rc != WC_EXE_ABORT)
			break;
		// let's invoke the device finish functions
		if (wc->cbs.on_device_finish) {
			uint32_t idx;
			for (idx = 0; idx < wc->ocl.device_max; ++idx) {
				wc_cldev_t *dev = &(wc->ocl.devices[idx]);
				rc = wc->cbs.on_device_finish(wc, dev, idx, wc->cbs.user,
											&wc->globaldata);
				if (rc != WC_EXE_OK) {
					WC_ERROR("Device %u returned error: %d\n", idx, rc);
					break;
				}
			}
			if (rc != WC_EXE_OK)
				break;
		}
		wc->state = WC_EXECSTATE_DEVICE_FINISHED;
	} while (0);
	// if rc had an earlier value keep that
	rc |= wc_executor_post_run(wc);
	return rc;
}

static const char *wc_devtype_to_string(const wc_devtype_t devt)
{
#undef DEV2STR
#define DEV2STR(A) case A: return #A
	switch (devt) {
	DEV2STR(WC_DEVTYPE_CPU);
	DEV2STR(WC_DEVTYPE_GPU);
	DEV2STR(WC_DEVTYPE_ANY);
	default: return "unknown";
	}
#undef DEV2STR
}

void wc_executor_dump(const wc_exec_t *wc)
{
	if (wc) {
		if (wc->mpi_initialized) {
			WC_INFO("MPI has been initialized successfully.\n");
		}
		WC_INFO("No, of Systems: %d\n", wc->num_systems);
		WC_INFO("My System Id: %d\n", wc->system_id);
		if (wc->ocl_initialized) {
			WC_INFO("OpenCL has been initialized successfully.\n");
			wc_opencl_dump(&(wc->ocl));
		}
		if (wc->callbacks_set) {
			WC_INFO("Callbacks have been set.\n");
			WC_INFO("Max Devices: %u\n", wc->cbs.max_devices);
			WC_INFO("Device Type: %s\n",
					wc_devtype_to_string(wc->cbs.device_type));
		}
	}
}

uint64_t wc_executor_num_tasks(const wc_exec_t *wc)
{
	return (wc) ? wc->num_tasks : 0;
}

uint32_t wc_executor_num_devices(const wc_exec_t *wc)
{
	if (wc && wc->ocl_initialized) {
		return wc->ocl.device_max;
	}
	return 0;
}
