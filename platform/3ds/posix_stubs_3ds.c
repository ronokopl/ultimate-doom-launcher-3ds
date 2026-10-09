/* POSIX process/user calls newlib declares but the 3DS cannot provide. Subsystems sheet: posix_stubs.
 * Callers: cmdlib.cpp NicePath (~user expansion), music_midi_timidity.cpp (external TiMidity process).
 * Every stub reports failure, so the engine takes its existing "not available" paths. */
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#include <wordexp.h>
#include <sys/wait.h>

uid_t getuid (void) { return 0; }
struct passwd *getpwuid (uid_t uid) { (void)uid; return NULL; }
struct passwd *getpwnam (const char *name) { (void)name; return NULL; }
int pipe (int fildes[2]) { (void)fildes; errno = ENOSYS; return -1; }
int execvp (const char *file, char * const argv[]) { (void)file; (void)argv; errno = ENOSYS; return -1; }
pid_t waitpid (pid_t pid, int *status, int options) { (void)pid; (void)status; (void)options; errno = ECHILD; return -1; }
int wordexp (const char *__restrict words, wordexp_t *__restrict we, int flags)
{
	(void)words; (void)flags;
	we->we_wordc = 0;
	we->we_wordv = NULL;
	return WRDE_NOSPACE;
}
void wordfree (wordexp_t *we) { (void)we; }
