#include <sys/types.h>
#include <sys/param.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <bmd_plugin.h>

static int
exec_qemu(struct vm *vm, nvlist_t *pl_conf)
{
	struct vm_conf *conf = vm_get_conf(vm);
	struct disk_conf *dc;
	struct iso_conf *ic;
	struct net_conf *nc;
	pid_t pid;
	int infd[2], outfd[2], errfd[2];
	char **args;
	char *buf = NULL;
	size_t n, buf_size;
	ssize_t rc;
	FILE *fp;
	bool dopipe = ((get_assigned_comport(vm) == NULL) ||
		       (strcasecmp(get_assigned_comport(vm), "stdio") != 0));

	if (dopipe) {
		if (pipe(infd) < 0) {
			return -1;
		}
		if (pipe(outfd) < 0) {
			close(infd[0]);
			close(infd[1]);
			return -1;
		}

		if (pipe(errfd) < 0) {
			close(infd[0]);
			close(infd[1]);
			close(outfd[0]);
			close(outfd[1]);
			return -1;
		}
	}

	pid = fork();
	if (pid > 0) {
		/* parent process */
		if (dopipe) {
			close(infd[1]);
			close(outfd[1]);
			close(errfd[1]);
			set_infd(vm, infd[0]);
			set_outfd(vm, outfd[0]);
			set_errfd(vm, errfd[0]);
			if (is_fbuf_enable(conf)) {
				buf_size = asprintf(&buf,
				    "set_password vnc %s\n",
				    get_fbuf_password(conf));
				n = 0;
				while (n < buf_size) {
					if ((rc = write(get_infd(vm), buf + n,
						 buf_size - n)) < 0)
						if (errno != EINTR &&
						    errno != EAGAIN)
							break;
					if (rc > 0)
						n += rc;
				}
				free(buf);
			}
		}
		set_pid(vm, pid);
		set_state(vm, RUN);
	} else if (pid == 0) {
		/* child process */
		if (dopipe) {
			close(infd[0]);
			close(outfd[0]);
			close(errfd[0]);
			dup2(infd[1], 0);
			dup2(outfd[1], 1);
			dup2(errfd[1], 2);
		}

		fp = open_memstream(&buf, &buf_size);
		if (fp == NULL) {
			exit(1);
		}
		flockfile(fp);

		fprintf(fp, LOCALBASE "/bin/qemu-system-%s\n-accel\n-tcg\n",
			  nvlist_get_string(pl_conf, "qemu_arch"));
		if (nvlist_exists_string(pl_conf, "qemu_machine"))
			fprintf(fp, "-machine\n%s\n",
				nvlist_get_string(pl_conf, "qemu_machine"));
		fprintf(fp, "-rtc\n");
		fprintf(fp, "base=%s\n", is_utctime(conf) ? "utc" : "localtime");
		if (get_debug_port(conf) != NULL)
			fprintf(fp, "-gdb\ntcp::%s\n", get_debug_port(conf));
		fprintf(fp, "-smp\n%d\n", get_ncpu(conf));
		fprintf(fp, "-m %s\n", get_memory(conf));
		if (get_assigned_comport(vm) == NULL) {
			fprintf(fp, "-monitor\n-stdio\n");
		} else if (strcasecmp(get_assigned_comport(vm), "stdio") == 0) {
			fprintf(fp, "-chardev\n"
				"stdio,mux=on,id=char0,signal=off\n"
				"-mon\n"
				"chardev=char0,mode=readline\n"
				"-serial\n"
				"chardev:char0\n");
		} else {
			fprintf(fp, "-monitor\n"
				"stdio\n"
				"-chardev\n"
				"serial,path=%s,id=char0,signal=off\n"
				"-serial\n"
				"chardev:char0",
				get_assigned_comport(vm));
		}

		fprintf(fp, "-boot\n%s\n", is_install(conf) ? "d" : "c");

		int i = 0;
		DISK_CONF_FOREACH (dc, conf) {
			char *path = get_disk_conf_path(dc);
			char *type = get_disk_conf_type(dc);
			fprintf(fp, "-blockdev\n");
			if (strncmp(path, "/dev/", 4) == 0) {
				fprintf(fp,
				    "node-name=blk%d,driver=raw,file.driver=host_device,file.filename=%s\n",
				    i, path);
			} else {
				fprintf(fp,
				    "node-name=blk%d,driver=file,filename=%s\n",
				    i++, path);
			}
			fprintf(fp, "-device\n");
			fprintf(fp, "%s,drive=blk%d\n", type, i);
			i++;
		}
		ic = get_iso_conf(conf);
		if (ic != NULL) {
			fprintf(fp, "-cdrom\n%s\n", get_iso_conf_path(ic));
		}
		TAPS_FOREACH (nc, vm) {
			fprintf(fp, "-nic\n");
			fprintf(fp, "tap,ifname=%s\n", get_net_conf_tap(nc));
		}
		if (is_fbuf_enable(conf)) {
			fprintf(fp, "-vga\nstd\n-vnc\n:%d\n",
				get_fbuf_port(conf) - 5900);
		}
		if (is_mouse(conf)) {
			fprintf(fp, "-usb\n");
		}
		fprintf(fp, "-name\n%s\n", get_name(conf));

		funlockfile(fp);
		fclose(fp);

		args = split_args(buf);
		if (args == NULL) {
			exit(1);
		}
		execv(args[0], args);
		exit(1);
	} else {
		exit(1);
	}

	return 0;
}

static int
start_qemu(struct vm *vm, nvlist_t *pl_conf)
{
	if (! nvlist_exists_string(pl_conf, "qemu_arch"))
		nvlist_add_string(pl_conf, "qemu_arch", "x86_64");

	return exec_qemu(vm, pl_conf);
}

static void
cleanup_qemu(struct vm *vm, nvlist_t *pl_conf __unused)
{
#define VM_CLOSE_FD(fd)                \
	do {                           \
		if (get_##fd(vm) != -1) {	\
			close(get_##fd(vm));	\
			set_##fd(vm, -1);	\
		}                      \
	} while (0)

	VM_CLOSE_FD(infd);
	VM_CLOSE_FD(outfd);
	VM_CLOSE_FD(errfd);
	VM_CLOSE_FD(logfd);
#undef VM_CLOSE_FD
}

static int
put_command(struct vm *vm, const char *cmd)
{
	ssize_t rc;
	size_t len, n;

	if (get_infd(vm) == -1)
		return 0;

	len = strlen(cmd);

	for (n = 0; n < len;) {
		if ((rc = write(get_infd(vm), cmd + n, len - n)) < 0)
			switch (errno) {
			case EINTR:
			case EAGAIN:
				continue;
			case EPIPE:
				close(get_infd(vm));
				set_infd(vm, -1);
				/* FALLTHROUGH */
			default:
				return -1;
			}
		n += rc;
	}

	return n;
}

static int
reset_qemu(struct vm *vm, nvlist_t *pl_conf __unused)
{
	return put_command(vm, "system_reset\n");
}

static int
poweroff_qemu(struct vm *vm, nvlist_t *pl_conf __unused)
{
	return put_command(vm, "quit\n");
}

static int
acpi_poweroff_qemu(struct vm *vm, nvlist_t *pl_conf __unused)
{
	return put_command(vm, "system_powerdown\n");
}

static int
compare_archs(const void *a, const void *b)
{
	return strcasecmp((const char *)a, *(const char * const *)b);
}

static int
set_conf_value(nvlist_t *config, const char *key, const char *val)
{
	if (nvlist_exists_string(config, key))
		nvlist_free_string(config, key);

	nvlist_add_string(config, key, val);

	return nvlist_error(config) != 0 ? -1 : 0;
}

static int
parse_qemu_arch(nvlist_t *config, const char *key, const char *val)
{
	const char **p,
	    *archs[] = { "aarch64", "alpha", "arm", "cris", "hppa", "i386",
		    "lm32", "m68k", "microblaze", "microblazeel", "mips",
		    "mips64", "mips64el", "mipsel", "moxie", "nios2", "or1k",
		    "ppc", "ppc64", "riscv32", "riscv64", "rx", "s390x", "sh4",
		    "sh4eb", "sparc", "sparc64", "tricore", "unicore32",
		    "x86_64", "xtensa", "xtensaeb" };

	if ((p = bsearch(val, archs, nitems(archs), sizeof(archs[0]),
			 compare_archs)) == NULL)
		return -1;

	return set_conf_value(config, key, *p);
}

static int
qemu_parse_config(nvlist_t *config, const char *key, const char *val)
{
	if (strcasecmp(key, "qemu_arch") == 0)
		return parse_qemu_arch(config, key, val);

	if (strcasecmp(key, "qemu_machine") == 0)
		return set_conf_value(config, key, val);

	return 1;
}

static struct vm_method qemu_method = {
	.name = "qemu",
	.vm_start = start_qemu,
	.vm_reset = reset_qemu,
	.vm_poweroff = poweroff_qemu,
	.vm_acpi_poweroff = acpi_poweroff_qemu,
	.vm_cleanup =  cleanup_qemu
};

PLUGIN_DESC plugin_desc = {
	.version = PLUGIN_VERSION,
	.name = "qemu",
	.initialize = NULL,
	.finalize = NULL,
	.on_status_change = NULL,
	.parse_config = qemu_parse_config,
	.method = &qemu_method,
	.on_reload_config = NULL,
};
