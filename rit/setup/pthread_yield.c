/* Fix 2 for VCS 2017.12 on RHEL 9: glibc 2.34 no longer lets new programs
 * link pthread_yield(), which VCS's save/restore object still calls. On Linux
 * glibc implemented it as sched_yield(), so provide that. install_deps.sh
 * compiles this to ~/opt/vcs_compat/pthread_yield.o. */
#include <sched.h>
int pthread_yield(void) { return sched_yield(); }
