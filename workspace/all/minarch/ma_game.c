#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <zip.h>
#include <libgen.h>
#include <sys/wait.h>

#include "ma_internal.h"
#include "ma_game.h"

struct Game game;
struct retro_disk_control_ext_callback disk_control_ext;

int extract_7z(char** extensions);
static int archiveEntryMatches(const char* entry_name, char** extensions);
static FILE* openReadPipe(char *const argv[], pid_t *pid_out);
static int closeReadPipe(FILE* pipe_file, pid_t pid);
static int runCommandToFd(char *const argv[], int output_fd);
static int waitForChildProcess(pid_t pid);
static unsigned int archiveCacheHash(const char* str);
static int prepareArchiveTempPath(const char* entry_name);
static int openArchiveOutputFile(const char* path, off_t expected_size);
static int extract_7z_with_bsdtar(char** extensions);
static int extract_7z_with_7zip(char** extensions, const char* command);

void Game_open(char* path) {
	LOG_info("Game_open\n");
	memset(&game, 0, sizeof(game));

	strcpy((char*)game.path, path);
	strcpy((char*)game.name, strrchr(path, '/')+1);
	strcpy((char*)game.alt_name, game.name); // default it

	// If we have a supported archive file, extract it when the core can't read it natively.
	if (suffixMatch(".zip", game.path) || suffixMatch(".7z", game.path)) {
		const char* archive_ext = suffixMatch(".7z", game.path) ? "7z" : "zip";
		int supports_archive = 0;
		int i = 0;
		char* ext;
		char exts[128];
		char* extensions[32];
		strcpy(exts,core.extensions);
		while ((ext=strtok(i?NULL:exts,"|"))) {
			extensions[i++] = ext;
			if (!strcmp(archive_ext, ext))
				supports_archive = 1;
		}
		extensions[i] = NULL;

		// if the core doesn't support archive files natively
		if (!supports_archive) {
			LOG_info("Extracting %s file manually: %s\n", archive_ext, game.path);
			if ((exactMatch(archive_ext, "zip") && !extract_zip(extensions)) ||
				(exactMatch(archive_ext, "7z") && !extract_7z(extensions)))
				return;
			// Update the game name to the extracted file name instead of the zip name
			if (CFG_getUseExtractedFileName())
				strcpy((char*)game.alt_name, strrchr(game.tmp_path, '/')+1);
		}
		else {
			LOG_info("Core can handle %s file: %s\n", archive_ext, game.path);
		}
	}

	// some cores handle opening files themselves, eg. pcsx_rearmed
	// if the frontend tries to load a 500MB file itself bad things happen
	if (!core.need_fullpath) {
		path = game.tmp_path[0]=='\0'?game.path:game.tmp_path;

		FILE *file = fopen(path, "r");
		if (file==NULL) {
			LOG_error("Error opening game: %s\n\t%s\n", path, strerror(errno));
			return;
		}

		fseek(file, 0, SEEK_END);
		game.size = ftell(file);

		rewind(file);
		game.data = malloc(game.size);
		if (game.data==NULL) {
			LOG_error("Couldn't allocate memory for file: %s\n", path);
			return;
		}

		fread(game.data, sizeof(uint8_t), game.size, file);

		fclose(file);
	}

	// m3u-based?
	char* tmp;
	char m3u_path[256];
	char base_path[256];
	char dir_name[256];

	strcpy(m3u_path, game.path);
	tmp = strrchr(m3u_path, '/') + 1;
	tmp[0] = '\0';

	strcpy(base_path, m3u_path);

	tmp = strrchr(m3u_path, '/');
	tmp[0] = '\0';

	tmp = strrchr(m3u_path, '/');
	strcpy(dir_name, tmp);

	tmp = m3u_path + strlen(m3u_path);
	strcpy(tmp, dir_name);

	tmp = m3u_path + strlen(m3u_path);
	strcpy(tmp, ".m3u");

	if (exists(m3u_path)) {
		strcpy(game.m3u_path, m3u_path);
		strcpy((char*)game.name, strrchr(m3u_path, '/')+1);
		strcpy((char*)game.alt_name, game.name); // default it
	}

	game.is_open = 1;
}

void Game_close(void) {
	if (game.data) free(game.data);
	// why delete tempfile? keep it for next time when loading the game its much faster from /tmp ram folder
	// if (game.tmp_path[0]) remove(game.tmp_path);
	game.is_open = 0;
	VIB_setStrength(0); // just in case
}

void Game_changeDisc(char* path) {

	if (exactMatch(game.path, path) || !exists(path)) return;

	Game_close();
	Game_open(path);

	struct retro_game_info game_info = {};
	game_info.path = game.path;
	game_info.data = game.data;
	game_info.size = game.size;

	disk_control_ext.replace_image_index(0, &game_info);
	putFile(CHANGE_DISC_PATH, path); // NextUI still needs to know this to update recents.txt
}

int extract_zip(char** extensions)
{
	char buf[100];
	struct zip *za;
	int ze;
	if ((za = zip_open(game.path, 0, &ze)) == NULL) {
		zip_error_t error;
		zip_error_init_with_code(&error, ze);
		LOG_error("can't open zip archive `%s': %s\n", game.path, zip_error_strerror(&error));
		return 0;
	}

	mkdir("/tmp/nextarch",0777);
	char tmp_dirname[255];
	snprintf(tmp_dirname, sizeof(tmp_dirname), "%s/%s", "/tmp/nextarch",core.tag);
	mkdir(tmp_dirname,0777);

	int i, len;
	int fd;
	struct zip_file *zf;
	struct zip_stat sb;
	long long sum;
	for (i = 0; i < zip_get_num_entries(za, 0); i++) {
		if (zip_stat_index(za, i, 0, &sb) == 0) {
			len = strlen(sb.name);
			if (sb.name[len - 1] == '/') {
				sprintf(game.tmp_path, "%s/%s", tmp_dirname, basename((char*)sb.name));
			} else {
				int found = 0;
				char extension[8];
				for (int e=0; extensions[e]; e++) {
					sprintf(extension, ".%s", extensions[e]);
					if (suffixMatch(extension, sb.name)) {
						found = 1;
						break;
					}
				}
				if (!found) continue;

				sprintf(game.tmp_path, "%s/%s", tmp_dirname, basename((char*)sb.name));

				// Check if file already exists and has the correct size
				struct stat st;
				if (stat(game.tmp_path, &st) == 0 && st.st_size == sb.size) {
					// File already exists with correct size, skip extraction
					LOG_info("File already exists with correct size, skipping extraction: %s\n", game.tmp_path);
					return 1;
				}

				zf = zip_fopen_index(za, i, 0);
				if (!zf) {
					LOG_error( "zip_fopen_index failed\n");
					return 0;
				}

				// Try to create file exclusively first to avoid race condition
				fd = open(game.tmp_path, O_RDWR | O_CREAT | O_EXCL, 0644);
				if (fd < 0) {
					if (errno == EEXIST) {
						// File was created by another process, verify it's complete
						zip_fclose(zf);
						if (stat(game.tmp_path, &st) == 0 && st.st_size == sb.size) {
							LOG_info("File was created by another process, using it: %s\n", game.tmp_path);
							return 1;
						}
						// File exists but wrong size, try to truncate and rewrite
						fd = open(game.tmp_path, O_RDWR | O_TRUNC, 0644);
						if (fd < 0) {
							LOG_error("open failed after EEXIST: %s\n", strerror(errno));
							return 0;
						}
						zf = zip_fopen_index(za, i, 0);
						if (!zf) {
							LOG_error("zip_fopen_index failed on retry\n");
							close(fd);
							return 0;
						}
					} else {
						LOG_error("open failed: %s\n", strerror(errno));
						zip_fclose(zf);
						return 0;
					}
				}

				sum = 0;
				while (sum != sb.size) {
					len = zip_fread(zf, buf, 100);
					if (len < 0) {
						LOG_error( "zip_fread failed\n");
						close(fd);
						zip_fclose(zf);
						return 0;
					}
					write(fd, buf, len);
					sum += len;
				}
				close(fd);
				zip_fclose(zf);
				return 1;
			}
		}
	}

	if (zip_close(za) == -1) {
		LOG_error("can't close zip archive `%s'\n", game.path);
		return 0;
	}

	return 0;
}

static int archiveEntryMatches(const char* entry_name, char** extensions)
{
	char extension[32];
	for (int e = 0; extensions[e]; e++) {
		snprintf(extension, sizeof(extension), ".%s", extensions[e]);
		if (suffixMatch(extension, entry_name))
			return 1;
	}
	return 0;
}

static int waitForChildProcess(pid_t pid)
{
	int status;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno == EINTR)
			continue;
		return -1;
	}

	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
}

static FILE* openReadPipe(char *const argv[], pid_t *pid_out)
{
	int pipefd[2];
	if (pipe(pipefd) < 0)
		return NULL;

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return NULL;
	}

	if (pid == 0) {
		int nullfd = open("/dev/null", O_WRONLY);
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		if (nullfd >= 0) {
			dup2(nullfd, STDERR_FILENO);
			if (nullfd > STDERR_FILENO)
				close(nullfd);
		}
		close(pipefd[1]);
		execvp(argv[0], argv);
		_exit(errno == ENOENT ? 127 : 126);
	}

	close(pipefd[1]);
	FILE* pipe_file = fdopen(pipefd[0], "r");
	if (!pipe_file) {
		close(pipefd[0]);
		waitForChildProcess(pid);
		return NULL;
	}

	*pid_out = pid;
	return pipe_file;
}

static int closeReadPipe(FILE* pipe_file, pid_t pid)
{
	fclose(pipe_file);
	return waitForChildProcess(pid);
}

static int runCommandToFd(char *const argv[], int output_fd)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		int nullfd = open("/dev/null", O_WRONLY);
		if (dup2(output_fd, STDOUT_FILENO) < 0)
			_exit(126);
		if (nullfd >= 0) {
			dup2(nullfd, STDERR_FILENO);
			if (nullfd > STDERR_FILENO)
				close(nullfd);
		}
		execvp(argv[0], argv);
		_exit(errno == ENOENT ? 127 : 126);
	}

	return waitForChildProcess(pid);
}

static unsigned int archiveCacheHash(const char* str)
{
	unsigned int hash = 2166136261u;
	while (*str) {
		hash ^= (unsigned char)*str++;
		hash *= 16777619u;
	}
	return hash;
}

static int prepareArchiveTempPath(const char* entry_name)
{
	char tmp_dirname[MAX_PATH];
	char archive_dirname[MAX_PATH];
	unsigned int archive_hash = archiveCacheHash(game.path);
	unsigned int entry_hash = archiveCacheHash(entry_name);

	mkdir("/tmp/nextarch", 0777);
	snprintf(tmp_dirname, sizeof(tmp_dirname), "%s/%s", "/tmp/nextarch", core.tag);
	mkdir(tmp_dirname, 0777);

	if (snprintf(archive_dirname, sizeof(archive_dirname), "%s/%08x-%08x", tmp_dirname, archive_hash, entry_hash) >= sizeof(archive_dirname)) {
		LOG_error("Archive cache dir path too long for `%s`\n", entry_name);
		game.tmp_path[0] = '\0';
		return 0;
	}
	mkdir(archive_dirname, 0777);

	if (snprintf(game.tmp_path, sizeof(game.tmp_path), "%s/%s", archive_dirname, baseName(entry_name)) >= sizeof(game.tmp_path)) {
		LOG_error("Archive temp path too long for `%s`\n", entry_name);
		game.tmp_path[0] = '\0';
		return 0;
	}

	return 1;
}

static int openArchiveOutputFile(const char* path, off_t expected_size)
{
	struct stat st;
	int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd >= 0)
		return fd;

	if (errno != EEXIST)
		return -1;

	if (expected_size >= 0 && stat(path, &st) == 0 && st.st_size == expected_size)
		return -2;

	return open(path, O_RDWR | O_TRUNC | O_CREAT, 0644);
}

static int extract_7z_with_bsdtar(char** extensions)
{
	pid_t pid;
	char entry_name[MAX_PATH] = {0};
	char line[1024];
	int found_entry = 0;
	char *list_argv[] = {"bsdtar", "-tf", game.path, NULL};
	FILE* pipe_file = openReadPipe(list_argv, &pid);
	if (!pipe_file)
		return 0;

	while (fgets(line, sizeof(line), pipe_file) != NULL) {
		normalizeNewline(line);
		trimTrailingNewlines(line);
		if (!line[0] || suffixMatch("/", line))
			continue;
		if (found_entry || !archiveEntryMatches(line, extensions))
			continue;
		snprintf(entry_name, sizeof(entry_name), "%s", line);
		found_entry = 1;
	}

	if (closeReadPipe(pipe_file, pid) != 0 || !entry_name[0])
		return 0;

	if (!prepareArchiveTempPath(entry_name))
		return 0;

	int fd = openArchiveOutputFile(game.tmp_path, -1);
	if (fd == -2)
		return 1;
	if (fd < 0)
		return 0;

	char *extract_argv[] = {"bsdtar", "-x", "-O", "-f", game.path, entry_name, NULL};
	int status = runCommandToFd(extract_argv, fd);
	close(fd);

	struct stat st;
	if (status != 0 || stat(game.tmp_path, &st) != 0 || st.st_size <= 0) {
		unlink(game.tmp_path);
		return 0;
	}

	return 1;
}

static int extract_7z_with_7zip(char** extensions, const char* command)
{
	pid_t pid;
	char entry_name[MAX_PATH] = {0};
	char matched_entry_name[MAX_PATH] = {0};
	char line[1024];
	int matched_entry = 0;
	int found_entry = 0;
	off_t entry_size = -1;
	char *list_argv[] = {(char*)command, "l", "-slt", "--", game.path, NULL};
	FILE* pipe_file = openReadPipe(list_argv, &pid);
	if (!pipe_file)
		return 0;

	while (fgets(line, sizeof(line), pipe_file) != NULL) {
		normalizeNewline(line);
		trimTrailingNewlines(line);
		if (prefixMatch("Path = ", line)) {
			char *path = line + strlen("Path = ");
			matched_entry = !found_entry && archiveEntryMatches(path, extensions);
			if (matched_entry)
				snprintf(matched_entry_name, sizeof(matched_entry_name), "%s", path);
			continue;
		}

		if (!matched_entry || found_entry || !prefixMatch("Size = ", line))
			continue;

		char *size = line + strlen("Size = ");
		char *endptr;
		long long parsed_size = strtoll(size, &endptr, 10);
		if (endptr == size || parsed_size < 0)
			continue;
		snprintf(entry_name, sizeof(entry_name), "%s", matched_entry_name);
		entry_size = (off_t)parsed_size;
		found_entry = 1;
		matched_entry = 0;
	}

	if (closeReadPipe(pipe_file, pid) != 0 || !entry_name[0] || entry_size < 0)
		return 0;

	if (!prepareArchiveTempPath(entry_name))
		return 0;

	int fd = openArchiveOutputFile(game.tmp_path, entry_size);
	if (fd == -2)
		return 1;
	if (fd < 0)
		return 0;

	char *extract_argv[] = {(char*)command, "e", "-so", "-y", "--", game.path, entry_name, NULL};
	int status = runCommandToFd(extract_argv, fd);
	close(fd);

	struct stat st;
	if (status != 0 || stat(game.tmp_path, &st) != 0 || st.st_size <= 0) {
		unlink(game.tmp_path);
		return 0;
	}

	return 1;
}

int extract_7z(char** extensions)
{
	if (!exactMatch("desktop", PLATFORM)) {
		if (extract_7z_with_7zip(extensions, BIN_PATH "/7zz"))
			return 1;

		LOG_error("can't extract 7z archive `%s' with bundled helper `%s`\n", game.path, BIN_PATH "/7zz");
		return 0;
	}

	static const char* seven_zip_commands[] = {
		BIN_PATH "/7zz",
		"7zz",
		"7z",
		"7za",
		"7zr",
		NULL
	};

	for (int i = 0; seven_zip_commands[i]; i++) {
		if (extract_7z_with_7zip(extensions, seven_zip_commands[i]))
			return 1;
	}

	if (extract_7z_with_bsdtar(extensions))
		return 1;

	LOG_error("can't extract 7z archive `%s'\n", game.path);
	return 0;
}
