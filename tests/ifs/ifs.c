/*
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 *
 * Run 'In Field Scan' test provided by the Linux kernel on compatible hardware
 *
 * Requires `ifs.ko` to be loaded in the Linux kernel, as well as
 * firmware test blob data in `/lib/firmware/...`. Supported since
 * 6.2
 *
 */

#define _GNU_SOURCE 1
#include <sandstone.h>

#if defined(__x86_64__) && defined(__linux__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <paths.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#define PATH_SYS_IFS_BASE "/sys/devices/virtual/misc/"
#define DEFAULT_TEST_ID 1

#define BUFLEN 256 // kernel module prints at most a 64bit value

/* from linux/ifs/ifs.h: */
/*
 * Driver populated error-codes
 * 0xFD: Test timed out before completing all the chunks.
 * 0xFE: not all scan chunks were executed. Maximum forward progress retries exceeded.
 */
#define IFS_SW_TIMEOUT                          0xFD
#define IFS_SW_PARTIAL_COMPLETION               0xFE

typedef struct {
    char image_id[BUFLEN];
    char image_version[BUFLEN];
} ifs_test_t;

static bool is_result_code_skip(unsigned long long code)
{
    switch (code) {
    case IFS_SW_TIMEOUT:
    case IFS_SW_PARTIAL_COMPLETION:
        return true;
    }

    return false;
}

static bool write_file(int dfd, const char *filename, const char* value)
{
        size_t l = strlen(value);
        int fd = openat(dfd, filename, O_WRONLY | O_CLOEXEC);
        if (fd == -1)
                return false;
        if (write(fd, value, l) != l) {
                close(fd);
                return false;
        }
        close(fd);
        return true;
}

static ssize_t read_file_fd(int fd, char buf[static restrict BUFLEN])
{
        ssize_t n = read(fd, buf, BUFLEN);
        close(fd);

        /* trim newlines */
        while (n > 0 && buf[n - 1] == '\n') {
                buf[n - 1] = '\0';
                --n;
        }
        return n;
}

static ssize_t read_file(int dfd, const char *filename, char buf[static restrict BUFLEN])
{
        int fd = openat(dfd, filename, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return fd;

        return read_file_fd(fd, buf);
}

static bool load_test_file(int dfd, int batch_fd, struct test *test, ifs_test_t *ifs_info)
{
    char current_buf[BUFLEN], status_buf[BUFLEN];
    int next_test, current_test, enforce_run;

    /* read both files status and current_batch */
    read_file(dfd, "status", status_buf);
    read_file_fd(batch_fd, current_buf);

    /* when previous run has a status of fail, skip test */
    enforce_run = get_testspecific_knob_value_uint(test, "enforce_run", -1);
    if (memcmp(status_buf, "fail", strlen("fail")) == 0 && enforce_run != 1 )
    {
        log_warning("Previous run failure found! Refusing to run");
        return false;
    }

    /* get interactive test file if provided by user */
    next_test = get_testspecific_knob_value_uint(test, "test_file", -1);
    if (next_test == -1)
    {
        if (memcmp(current_buf, "none", strlen("none")) == 0)
            next_test = DEFAULT_TEST_ID;
        else
        {
            current_test = (int) strtoul(current_buf, NULL, 0);
            if (current_test < 0 && errno == ERANGE)
            {
                log_info("Cannot parse current_batch value: %s", current_buf);
                return false;
            }

            if (memcmp(status_buf, "untested", strlen("untested")) == 0)
            {
                log_info("Test file %s remains untested, so try again", current_buf);
                next_test = current_test;
            }
            else
                next_test = current_test + 1;
        }
    }

    /* write next test file ID */
    sprintf(ifs_info->image_id, "%#x", next_test);
    if (write_file(dfd, "current_batch", ifs_info->image_id))
    {
        return true;
    }
    else if (errno == ENOENT)
    {
        /* when next blob does not exists, it will fail with
         * ENOENT error. Then, start from the begining */
        log_info("Test file %s, does not exists. Starting over from 0x%x", ifs_info->image_id, DEFAULT_TEST_ID);
        sprintf(ifs_info->image_id, "%#x", DEFAULT_TEST_ID);
        if (write_file(dfd, "current_batch", ifs_info->image_id))
        {
            return true;
        }
    }

    return false;
}

static int scan_init(struct test *test)
{
        /* allocate info struct */
        ifs_test_t *ifs_info = malloc(sizeof(ifs_test_t));

        /* see if driver is loaded, otherwise try to load it */
        int ifs0 = open(PATH_SYS_IFS_BASE "intel_ifs_0", O_DIRECTORY | O_PATH | O_CLOEXEC);
        if (ifs0 < 0) {
                /* modprobe kernel driver, ignore errors entirely here */
                pid_t pid = fork();
                if (pid == 0) {
                        execl("/sbin/modprobe", "/sbin/modprobe", "-q", "intel_ifs", NULL);

                        /* don't print an error if /sbin/modprobe wasn't found, but
                           log_debug() is fine (since the parent is waiting, we can
                           write to the FILE* because it's unbuffered) */
                        log_debug("Failed to run modprobe: %s", strerror(errno));
                        _exit(errno);
                } else if (pid > 0) {
                        /* wait for child */
                        int status, ret;
                        do {
                            ret = waitpid(pid, &status, 0);
                        } while (ret < 0 && errno == EINTR);
                } else {
                        /* ignore failure to fork() -- extremely unlikely */
                }

                /* try opening again now that we've potentially modprobe'd */
                ifs0 = open(PATH_SYS_IFS_BASE "intel_ifs_0", O_DIRECTORY | O_PATH | O_CLOEXEC);
        }

        /* see if we can open run_test and current_batch for writing */
        int run_fd = openat(ifs0, "run_test", O_WRONLY);
        int saved_errno = errno;
        if (run_fd < 0) {
                log_info("could not open intel_ifs_0/run_test for writing (not running as root?): %m");
                close(run_fd);
                return -saved_errno;
        }

        int batch_fd = openat(ifs0, "current_batch", O_RDWR);
        saved_errno = errno;
        if (batch_fd < 0) {
                log_info("could not open intel_ifs_0/current_batch for writing (not running as root?): %m");
                close(batch_fd);
                return -saved_errno;
        }
        close(run_fd);

        /* load test file */
        if (!load_test_file(ifs0, batch_fd, test, ifs_info)) {
        	log_skip(RESOURCE_UNAVAILABLE, "cannot load test file");
        	return EXIT_SKIP;
        }

        /* read image version if available and log it */
        if (read_file(ifs0, "image_version", ifs_info->image_version) <= 0) {
                strncpy(ifs_info->image_version, "unknown", BUFLEN);
        }
        log_info("Test image ID: %s version: %s", ifs_info->image_id, ifs_info->image_version);

        test->data = ifs_info;
        close(ifs0);
        return EXIT_SUCCESS;
}

static int scan_run(struct test *test, int cpu)
{
        char result[BUFLEN], my_cpu[BUFLEN];
        DIR *base;
        int basefd;
        bool any_test_succeded = false;
        ifs_test_t *ifs_info = test->data;

        if (cpu_info[cpu].thread_id != 0) {
			log_skip(RUNTIME_SKIP, "Test should run only on thread 0 on every core");
			return EXIT_SKIP;
		}

        basefd = open(PATH_SYS_IFS_BASE, O_DIRECTORY | O_CLOEXEC);
        if (basefd < 0)
                return -errno;      // shouldn't happen
        base = fdopendir(basefd);
        if (base == NULL)
            return -errno;          // shouldn't happen

        snprintf(my_cpu, sizeof(my_cpu), "%d\n", cpu_info[cpu].cpu_number);

        struct dirent *ent;
        while ((ent = readdir(base)) != NULL) {
                static const char prefix[] = "intel_ifs_";
                const char *d_name = ent->d_name;
                if (ent->d_type != DT_DIR || memcmp(ent->d_name, prefix, strlen(prefix)) != 0)
                        continue;

                int ifsfd = openat(basefd, d_name, O_DIRECTORY | O_PATH | O_CLOEXEC);
                if (ifsfd < 0) {
                        log_warning("Could not start test for \"%s\": %m", d_name);
                        continue;
                }

                /* start the test; this blocks until the test has finished */
                if (!write_file(ifsfd, "run_test", my_cpu)) {
                        log_warning("Could not start test for \"%s\": %m", d_name);
                        close(ifsfd);
                        continue;
                }

                /* read result */
                if (read_file(ifsfd, "status", result) < 0) {
                        log_warning("Could not obtain result for \"%s\": %m", d_name);
                        close(ifsfd);
                        continue;
                }

                if (memcmp(result, "fail", strlen("fail")) == 0) {
                        /* failed, get status code */
                        unsigned long long code;
                        ssize_t n = read_file(ifsfd, "details", result);
                        close(ifsfd);

                        if (n < 0) {
                                log_error("Test \"%s\" failed but could not retrieve error condition. Image ID: %s  version: %s", d_name, ifs_info->image_id, ifs_info->image_version);
                        } else {
                                if (sscanf(result, "%llx", &code) == 1 && is_result_code_skip(code)) {
                                        log_warning("Test \"%s\" did not run to completion, code: %s image ID: %s version: %s", d_name, result, ifs_info->image_id, ifs_info->image_version);
                                        continue;       // not a failure condition
                                }
                                log_error("Test \"%s\" failed with condition: %s image: %s version: %s", d_name, result, ifs_info->image_id, ifs_info->image_version);
                        }
                        break;
                } else if (memcmp(result, "pass", strlen("pass")) == 0) {
                        log_debug("Test \"%s\" passed", d_name);
                        any_test_succeded = true;
                }

                close(ifsfd);
        }

        closedir(base);
        return any_test_succeded ? EXIT_SUCCESS : EXIT_SKIP;
}

DECLARE_TEST(ifs, "Intel In-Field Scan (IFS) hardware selftest")
    .quality_level = TEST_QUALITY_PROD,
    .test_init = scan_init,
    .test_run = scan_run,
    .desired_duration = -1,
    .fracture_loop_count = -1,
END_DECLARE_TEST

#endif // __x86_64__ && __linux__
