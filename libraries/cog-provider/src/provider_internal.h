/**
 * @file provider_internal.h
 * @brief Private layout shared by cog-provider and its provider
 *        implementations.
 *
 * The public @c cog_provider_t is opaque.  The convenience wrappers in
 * provider.c (cog_provider_disconnect / cog_provider_send /
 * cog_provider_receive) must dispatch to the correct static vtable, so
 * every provider instance embeds a pointer to its vtable as its first
 * member.  Provider implementations allocate a struct whose first member
 * is a @c struct cog_provider and set @c vtable to their static table;
 * the wrappers then reach the vtable through the opaque pointer.
 *
 * This header is private: it lives in @c src/ and is not installed.
 * Consumers of cog-provider only ever see the opaque @c cog_provider_t.
 */
#ifndef COG_PROVIDER_INTERNAL_H
#define COG_PROVIDER_INTERNAL_H

#include <cog/provider_vtable.h>

/**
 * @brief Concrete layout of the opaque provider instance.
 *
 * The first member is the static vtable that created the instance.  It
 * must remain the first member so the convenience wrappers can dispatch
 * through the opaque pointer.
 */
struct cog_provider {
    const cog_provider_vtable_t *vtable; /**< Static vtable that owns this
                                              instance. */
};

#endif /* COG_PROVIDER_INTERNAL_H */
