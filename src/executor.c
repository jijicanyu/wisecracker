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

struct wc_executor_details {
	int peer_count;
	int peer_id;
	uint8_t mpi_initialized;
	wc_exec_callbacks_t cbs;
	wc_opencl_t ocl;
	uint8_t ocl_initialized;
	char *code;
	size_t codelen;
	char *buildopts;
};

wc_exec_t *wc_executor_init(int *argc, char ***argv, wc_devtype_t devt,
							uint32_t max_devices)
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
		memset(wc, 0, sizeof(wc_exec_t));
		rc = wc_mpi_init(argc, argv);
		if (rc != 0)
			break;
		wc->mpi_initialized = 1;
		wc->peer_count = wc_mpi_peer_count();
		if (wc->peer_count < 0) {
			rc = -1;
			break;
		}
		wc->peer_id = wc_mpi_peer_id();
		if (wc->peer_id < 0) {
			rc = -1;
			break;
		}
		wc->ocl_initialized = 0;
		rc = wc_opencl_init(devt, max_devices, &wc->ocl);
		if (rc < 0) {
			WC_ERROR("Failed to create local runtime on system\n");
			rc = -1;
			break;
		}
		wc->ocl_initialized = 1;
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
		if (wc->mpi_initialized) {
			rc = wc_mpi_finalize();
			if (rc != 0) {
				WC_WARN("MPI Finalize error.\n");
			}
			wc->mpi_initialized = 0;
		}
		if (wc->ocl_initialized) {
			wc_opencl_finalize(&wc->ocl);
			wc->ocl_initialized = 0;
		}
		WC_FREE(wc->code);
		WC_FREE(wc->buildopts);
		memset(wc, 0, sizeof(*wc));
		WC_FREE(wc);
	}
}

int wc_executor_peer_count(const wc_exec_t *wc)
{
	return (wc) ? wc->peer_count : WC_EXE_ERR_INVALID_PARAMETER;
}

int wc_executor_peer_id(const wc_exec_t *wc)
{
	return (wc) ? wc->peer_id : WC_EXE_ERR_INVALID_PARAMETER;
}

wc_err_t wc_executor_setup(wc_exec_t *wc, const wc_exec_callbacks_t *cbs)
{
	if (wc && cbs) {
		// verify if the required callbacks are present
		if (!cbs->get_code || !cbs->get_task_size ||
			!cbs->get_kernel_name) {
			WC_ERROR("Wisecracker needs the get_code, get_task_size and"
					" get_kernel_name callbacks.\n");
		} else {
			// copy the pointers into an internal structure
			memcpy(&(wc->cbs), cbs, sizeof(*cbs));
			return WC_EXE_OK;
		}
	}
	return WC_EXE_ERR_INVALID_PARAMETER;
}

enum {
	WC_EXECSTATE_NOT_STARTED = 0,
	WC_EXECSTATE_STARTED,
	WC_EXECSTATE_GOT_CODE,
	WC_EXECSTATE_GOT_BUILDOPTS,
	WC_EXECSTATE_FINISHED
};

wc_err_t wc_executor_run(wc_exec_t *wc, long timeout)
{
	int current_state;
	wc_err_t rc = WC_EXE_OK;
	if (!wc)
		return WC_EXE_ERR_INVALID_PARAMETER;
	do {
		current_state = WC_EXECSTATE_NOT_STARTED;
		// call the on_start event
		if (wc->cbs.on_start) {
			rc = wc->cbs.on_start(wc, wc->cbs.user);
			if (rc != WC_EXE_OK) {
				WC_ERROR("Error in on_start callback: %d\n", rc);
				break;
			}
		}
		current_state = WC_EXECSTATE_STARTED;
		// call the get_code function to retrieve the code
		if (!wc->cbs.get_code) {
			WC_ERROR("The get_code callback is missing\n");
			break;
		} else {
			wc->codelen = 0;
			wc->code = NULL;
			wc->code = wc->cbs.get_code(wc, wc->cbs.user, &wc->codelen);
			if (!wc->code || wc->codelen == 0) {
				WC_ERROR("Error in get_code callback: %d\n", rc);
				break;	
			}
		}
		current_state = WC_EXECSTATE_GOT_CODE;
		// call the build_opts function to retrieve the build options
		if (wc->cbs.get_build_options) {
			wc->buildopts = wc->cbs.get_build_options(wc, wc->cbs.user);
			if (!wc->buildopts) {
				WC_WARN("Build options returned was NULL.\n");
			}
		}
		current_state = WC_EXECSTATE_GOT_BUILDOPTS;
	} while (0);
	// call the on_finish event
	if (current_state != WC_EXECSTATE_NOT_STARTED && wc->cbs.on_finish) {
		rc = wc->cbs.on_finish(wc, wc->cbs.user);
		if (rc != WC_EXE_OK) {
			WC_ERROR("Error in on_finish callback: %d\n", rc);
		}
		current_state = WC_EXECSTATE_FINISHED;
	}
	return rc;
}
