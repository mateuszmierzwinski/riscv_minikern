/* procfiles — the generated files under /proc and /sys that describe the
 * running process and the machine, in the formats Linux software parses.
 */
#ifndef MINIKERN_KERNEL_PROCFILES_H
#define MINIKERN_KERNEL_PROCFILES_H

/* Registers every file here with procfs and the device nodes with synthfs. */
void procfiles_install(void);

#endif
