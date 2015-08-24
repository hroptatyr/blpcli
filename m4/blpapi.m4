dnl blpapi.m4 --- bloomberg platform API detection
dnl
dnl Copyright (C) 2014-2015 Sebastian Freundt
dnl
dnl Author: Sebastian Freundt <hroptatyr@fresse.org>
dnl
dnl Redistribution and use in source and binary forms, with or without
dnl modification, are permitted provided that the following conditions
dnl are met:
dnl
dnl 1. Redistributions of source code must retain the above copyright
dnl    notice, this list of conditions and the following disclaimer.
dnl
dnl 2. Redistributions in binary form must reproduce the above copyright
dnl    notice, this list of conditions and the following disclaimer in the
dnl    documentation and/or other materials provided with the distribution.
dnl
dnl 3. Neither the name of the author nor the names of any contributors
dnl    may be used to endorse or promote products derived from this
dnl    software without specific prior written permission.
dnl
dnl THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
dnl IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
dnl WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
dnl DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
dnl FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
dnl CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
dnl SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
dnl BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
dnl WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
dnl OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
dnl IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
dnl
dnl This file is part of blpcli

AC_DEFUN([AX_CHECK_BLPAPI], [dnl
## check for bloomberg's platform api
	AC_ARG_WITH([blpapi], [dnl
AS_HELP_STRING([--with-blpapi=PATH], [
Path to top level directory of blpapi.])],
		[with_blpapi="${withval}"], [with_blpapi="unset"])
	AC_ARG_VAR([blpapi_CFLAGS], [Compiler flags to use to include blpapi headers.])
	AC_ARG_VAR([blpapi_LIBS], [Linker flags to use to link to blpapi.])

	if test "${with_blpapi}" = "no"; then
		AC_MSG_ERROR([dnl
Not allowed to use blpapi.  However, this project is all about
code that links to the blpapi.  I see no point in continuing ...
])
	elif test "${blpapi_CFLAGS}${blpapi_LIBS}" = ""; then
		## preset blpapi_CFLAGS/blpapi_LIBS with the value provided
		## by --with-blpapi
		if test "${with_blpapi}" = "yes"; then
			## don't bother, user is an idiot
			:
		elif test "${with_blpapi}" = "unset"; then
			## they're still an idiot
			:
		else
			blpapi_CFLAGS="-I${with_blpapi}/include"
			blpapi_LIBS="-L${with_blpapi}/Linux -L${with_blpapi}/Darwin -L${with_blpapi}/SunOS -lblpapi3_64"
		fi
	fi

	## quick test if it's actually working
	AC_LANG_PUSH([C])
	AC_PROG_CC_C99

	save_CPPFLAGS="${CPPFLAGS}"
	CPPFLAGS="${CPPFLAGS} ${blpapi_CFLAGS}"

	AC_CHECK_HEADERS([blpapi_session.h])
	AC_CHECK_HEADERS([blpapi_request.h])
	AC_CHECK_HEADERS([blpapi_message.h])
	AC_CHECK_HEADERS([blpapi_event.h])
	AC_CHECK_HEADERS([blpapi_element.h])
	AC_CHECK_HEADERS([blpapi_correlationid.h])
	AC_CHECK_HEADERS([blpapi_name.h])
	AC_CHECK_HEADERS([blpapi_subscriptionlist.h])

	save_LDFLAGS="${LDFLAGS}"
	LDFLAGS="${LDFLAGS} ${blpapi_LIBS}"

	AC_CHECK_FUNCS([blpapi_Session_create])
	AC_CHECK_FUNCS([blpapi_Service_createRequest])

	LDFLAGS="${save_LDFLAGS}"
	CPPFLAGS="${save_CPPFLAGS}"
	AC_LANG_POP([C])
])dnl AX_CHECK_YUCK

dnl blpapi.m4 ends here
