#ifndef ACTRAISER_HLE_FATAL_H
#define ACTRAISER_HLE_FATAL_H

#if defined(_MSC_VER)
#define AR_HLE_NORETURN __declspec(noreturn)
#elif defined(__GNUC__) || defined(__clang__)
#define AR_HLE_NORETURN __attribute__((noreturn))
#else
#define AR_HLE_NORETURN _Noreturn
#endif

/* HLE invariant failures cannot return through the emulated CPU contract.
 * Production registers the game-coroutine escape before its first dispatch;
 * focused HLE tests leave it unset and retain abort-on-invalid-input behavior. */
typedef void (*ActRaiserHleFatalHostEscape)(const char *message);

void ActRaiserHleFatal_RegisterHostEscape(
    ActRaiserHleFatalHostEscape escape);

AR_HLE_NORETURN void ActRaiserHleFatal(const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#endif /* ACTRAISER_HLE_FATAL_H */
