/*
 * /sbin/launchd of the big system image: the first program the kernel runs.
 *
 * The system image (the Darwin files and the dyld shared cache) takes a minute
 * and a half to upload to the phone, so nothing that changes often lives in
 * it. All of that - the real init, its tests, the list of launchd jobs - is
 * in the small image that travels with the kernel (the second RAM disk,
 * /dev/md1), an APFS container (tools/apfs/mkapfs.py). This program
 * mounts it on /redwood and hands over.
 */
#include "mini.h"

int
main(int argc, char **argv, char **envp)
{
	char *init_argv[] = { "/redwood/sbin/init", NULL };
	int fd = open("/dev/console", O_RDWR, 0);

	(void)argc, (void)argv;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2) {
		close(fd);
	}
	struct stat target;
	if (stat("/storage-bootstrap", &target) == 0) {
		static const char outgoing[] = "/system/storage-bootstrap-old";
		static const char switch_args[] = "/system storage-bootstrap-old";
		if (mount("apfs", "/system", MNT_RDONLY, (void *)"/dev/ufs_system_image") != 0 ||
		    sysctlbyname("kern.apfs_synthetic", NULL, NULL, outgoing, sizeof(outgoing) - 1) != 0 ||
		    sysctlbyname("kern.hakios_switch_root", NULL, NULL, switch_args, sizeof(switch_args) - 1) != 0) {
			printf("stage0: storage root failed: %s\n", strerror(errno));
			goto halt;
		}
		chdir("/");
	}

	if (mount("apfs", "/redwood", MNT_RDONLY, (void *)"/dev/md1") != 0) {
		printf("stage0: cannot mount /dev/md1 on /hakios: %s\n", strerror(errno));
	} else {
		execve(init_argv[0], init_argv, envp);
		printf("stage0: cannot run %s: %s\n", init_argv[0], strerror(errno));
	}
halt:
	reboot(0x08 /* RB_HALT */);
	return 1;
}
