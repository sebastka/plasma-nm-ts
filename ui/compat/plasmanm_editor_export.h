/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * plasma-nm generates this header at build time and does not install it.
 * Consumers of libplasmanm_editor only need the visibility attribute.
 */

#ifndef PLASMANM_EDITOR_EXPORT_H
#define PLASMANM_EDITOR_EXPORT_H

#define PLASMANM_EDITOR_EXPORT __attribute__((visibility("default")))

#endif // PLASMANM_EDITOR_EXPORT_H
