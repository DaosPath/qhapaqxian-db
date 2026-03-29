/*
 * qhapaqxian_db_launcher.c
 *
 * Temporary Windows release-engineering bridge: provide a branded entrypoint
 * that forwards to the sibling postgres.exe built from the fork.
 */

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
find_postgres_sibling(char *target, size_t target_len)
{
	DWORD		len;
	char	   *slash;
	DWORD		attrs;

	len = GetModuleFileNameA(NULL, target, (DWORD) target_len);
	if (len == 0 || len >= target_len)
		return 0;

	slash = strrchr(target, '\\');
	if (slash == NULL)
		return 0;

	if (strcpy_s(slash + 1, target_len - (size_t) (slash + 1 - target),
				 "postgres.exe") != 0)
		return 0;

	attrs = GetFileAttributesA(target);
	if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
		return 0;

	return 1;
}

int
main(int argc, char **argv)
{
	char		target[MAX_PATH];
	char	  **child_argv;
	int			i;
	int			rc;

	if (!find_postgres_sibling(target, sizeof(target) / sizeof(target[0])))
	{
		fprintf(stderr,
				"QhapaqXian DB launcher could not find postgres.exe next to this executable.\n");
		return 1;
	}

	child_argv = (char **) calloc((size_t) argc + 1, sizeof(char *));
	if (child_argv == NULL)
	{
		fprintf(stderr, "QhapaqXian DB launcher ran out of memory.\n");
		return 1;
	}

	child_argv[0] = target;
	for (i = 1; i < argc; i++)
		child_argv[i] = argv[i];
	child_argv[argc] = NULL;

	rc = _spawnv(_P_WAIT, target, (const char * const *) child_argv);
	if (rc == -1)
	{
		fprintf(stderr, "QhapaqXian DB launcher failed to start postgres.exe.\n");
		free(child_argv);
		return 1;
	}

	free(child_argv);
	return rc;
}
