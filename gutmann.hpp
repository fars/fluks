/* Copyright (c) 2009, Markus Peloquin <markus@cs.wisc.edu>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS' AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#ifndef FLUKS_GUTMANN_HPP
#define FLUKS_GUTMANN_HPP

#include <sys/types.h>

#include <fstream>

#include "errors.hpp"

namespace fluks {

/** Securely erase data from a location on the hard disk.
 * \param file	The file object to be erased.
 * \param pos	The starting position in bytes.
 * \param bytes	The number of bytes to be erased.
 * \return	The same file object sent in.
 */
template <class Fstream>
Fstream	&gutmann_erase(Fstream &file, off_t pos, size_t bytes)
	    throw (Disk_error);

}

#include "gutmann_private.hpp"

#endif
