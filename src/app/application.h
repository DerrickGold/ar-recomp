#ifndef AR_APPLICATION_H
#define AR_APPLICATION_H
/* Process lifetime: launch paths, settings, immutable ROM, SDL and window.
 * Owns the reset loop; game_session owns each replaceable game lifetime. */

int Application_Run(int argc, char **argv);

#endif /* AR_APPLICATION_H */
