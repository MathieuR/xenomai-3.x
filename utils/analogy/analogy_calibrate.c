/**
 * @file
 * Analogy for Linux, calibration program
 *
 * @note Copyright (C) 2014 Jorge A. Ramirez-Ortiz <jro@xenomai.org>
 *
 * from original code from the Comedi project
 *
 * Xenomai is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <sys/time.h>
#include <sys/resource.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/mman.h>
#include <xeno_config.h>
#include <rtdm/analogy.h>
#include "analogy_calibrate.h"
#include "calibration_ni_m.h"


struct timespec calibration_start_time;
static const char *revision = "0.0.1";
a4l_desc_t descriptor;
FILE *cal = NULL;

static const struct option options[] = {
	{
#define help_opt	0
		.name = "help",
		.has_arg = 0,
		.flag = NULL,
	},
	{
#define device_opt	1
		.name = "device",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define output_opt	2
		.name = "output",
		.has_arg = 1,
		.flag = NULL,
	},
	{
		.name = NULL,
	}
};

static void print_usage(void)
{
	fprintf(stderr, "Usage: analogy_calibrate \n"
	       "  --help 	     		: this menu \n"
	       "  --device /dev/analogyX	: analogy device to calibrate \n"
	       "  --output filename   		: calibration results \n"
	      );
}

static void __attribute__ ((constructor)) __analogy_calibrate_init(void)
{
	clock_gettime(CLOCK_MONOTONIC, &calibration_start_time);
}

int main(int argc, char *argv[])
{
	struct sched_param param = {.sched_priority = 99};
	char *device = NULL, *file = NULL;
	int v, i, fd, err = 0;
	struct rlimit rl;

	__debug("version: git commit %s, revision %s \n", GIT_STAMP, revision);

	for (;;) {
		i = -1;
		v = getopt_long_only(argc, argv, "", options, &i);
		if (v == EOF)
			break;
		switch (i) {
		case help_opt:
			print_usage();
			exit(0);
		case device_opt:
			device = optarg;
			break;
		case output_opt:
			file = optarg;
			cal = fopen(file, "w+");
			if (!cal)
				error(EXIT, errno, "calibration file");
			__debug("calibration output: %s \n", file);
			break;
		default:
			print_usage();
			exit(EXIT_FAILURE);
		}
	}

	err = getrlimit(RLIMIT_STACK, &rl);
	if (!err) {
		if (rl.rlim_cur < rl.rlim_max) {
			rl.rlim_cur = rl.rlim_max;
			err = setrlimit(RLIMIT_STACK, &rl);
			if (err)
				__debug("setrlimit errno (%d) \n", errno);
			else
				__debug("Program Stack Size: %ld MB \n\n", rl.rlim_cur/(1024*1024));
		}
	}

	err = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (err < 0)
		error(EXIT, errno, "mlockall error");

	err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
	if (err)
		error(EXIT, 0, "pthread_setschedparam failed (0x%x)", err);

	fd = a4l_open(&descriptor, device);
	if (fd < 0)
		error(EXIT, 0, "open %s failed (%d)", device, fd);

	err = ni_m_board_supported(descriptor.driver_name);
	if (err)
		error(EXIT, 0, "board %s: driver %s not supported",
		      descriptor.board_name, descriptor.driver_name);

	/*
	 * TODO: modify the meaning of board/driver in the proc
	 */
	push_to_cal_file("[platform] \n");
	push_to_cal_file("driver_name: %s \n", descriptor.board_name);
	push_to_cal_file("board_name: %s \n", descriptor.driver_name);

	err = ni_m_software_calibrate();
	if (err)
		error(CONT, 0, "software calibration failed (%d)", err);

	a4l_close(&descriptor);
	if (cal)
		fclose(cal);

	return err;
}
