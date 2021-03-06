/**
 * Copyright (C) 2013 Red Hat, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Authors: Tomas Smetana <tsmetana@redhat.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <slp.h>
#include <Python.h>

#if PY_MAJOR_VERSION >= 3
#define PyInt_FromLong		PyLong_FromLong
#endif

struct _cb_cookie_s {
	PyObject *py_handle;
	PyObject *py_cookie;
	PyObject *py_callback;
};

typedef struct _cb_cookie_s cb_cookie_t;

/**
 * Translates the numeric error codes to strings for use in python exceptions.
 *
 * @param	SLPError code to be translated.
 * @return	Pointer to statically allocated message (do not free).
 */
static const char *get_slp_error_msg(SLPError err)
{
	static const char *err_msg[] = {
		[0] = "SLP_OK",
		[1] = "SLP_LANGUAGE_NOT_SUPPORTED",
		[2] = "SLP_PARSE_ERROR",
		[3] = "SLP_INVALID_REGISTRATION",
		[4] = "SLP_SCOPE_NOT_SUPPORTED",
		[6] = "SLP_AUTHENTICATION_ABSENT",
		[7] = "SLP_AUTHENTICATION_FAILED",
		[13] = "SLP_INVALID_UPDATE",
		[15] = "SLP_REFRESH_REJECTED",
		[17] = "SLP_NOT_IMPLEMENTED",
		[18] = "SLP_BUFFER_OVERFLOW",
		[19] = "SLP_NETWORK_TIMED_OUT",
		[20] = "SLP_NETWORK_INIT_FAILED",
		[21] = "SLP_MEMORY_ALLOC_FAILED",
		[22] = "SLP_PARAMETER_BAD",
		[23] = "SLP_NETWORK_ERROR",
		[24] = "SLP_INTERNAL_SYSTEM_ERROR",
		[25] = "SLP_HANDLE_IN_USE",
		[26] = "SLP_TYPE_ERROR"
	};

	/* The error codes are non-positive values with the exception of
	 * SLP_LAST_CALL (== 1). It is not an error in fact, but for the sake of
	 * completness, let's have it here as well. */
	if (err <= 0 && err >= -26)
		return err_msg[-err];
	else if (err == 1)
		return "SLP_LAST_CALL";
	else
		return "UNKNOWN_ERROR";
}

/**
 * Helper function to convert the python object to SLPHandle.
 *
 * @param py_handle The python handle.
 * @return SLPHandle or NULL on error.
 */
static inline SLPHandle get_slp_handle(PyObject *py_handle)
{
	SLPHandle hslp = NULL;

	if (PyCapsule_IsValid(py_handle, NULL))
		hslp = PyCapsule_GetPointer(py_handle, NULL);

	return hslp;
}

/**
 * Common part for all the callback functions; calls the python callback.
 * @param py_args	The python objects to be passed to the python callback.
 * @param cookie 	cb_cookie_t storing the python SLP handle, callback function
 * 					and the python callback cookie.
 * @param cleanup	If set to 1 then the return value of the python function is
 * 					not taken in account and the cookie is freed and the callback
 * 					reference count decreased. If set to 0 then the cleanup
 * 					happens only when the python callback returns "False".
 * @return	SLPBoolean value indicating if the callback wants to process more
 * 			data -- taken from the called python function.
 */
static inline SLPBoolean cb_common(PyObject *py_args, void *cookie, int cleanup)
{
	PyObject *py_result;
	cb_cookie_t *cb_data = (cb_cookie_t *) cookie;
	SLPBoolean ret;

	py_result = PyObject_CallObject(cb_data->py_callback, py_args);
	Py_DECREF(py_args);
	if (!py_result) {
		PyErr_SetString(PyExc_RuntimeError,
				"The SLP callback did not return a boolean value.");
		ret = 0;
	} else if (cleanup || !(ret = PyObject_IsTrue(py_result))) {
		Py_DECREF(cb_data->py_callback);
		free(cookie);
	}

	return ret;
}

/**
 * Callback for the SLPFindSrvs() as defined by RFC 2614.
 * @param hslp 		The language specific SLPHandle on which to register
 * 					the service.
 * @param srvurl 	Pointer to the SLP Service URL of the requested service.
 * @param lifetime 	The lifetime of the service in seconds.
 * @param errcode 	An error code indicating if an error occurred during
 * 					the operation.
 * @param cookie 	cb_cookie_t storing the python SLP handle, callback function
 * 					and the python callback cookie.
 * @return	SLPBoolean value indicating if the callback wants to process more
 * 			data.
 */
static SLPBoolean srv_url_cb(SLPHandle hslp, const char* srvurl,
		unsigned short lifetime, SLPError errcode, void* cookie)
{
	PyObject *py_args;
	cb_cookie_t *cb_data = (cb_cookie_t *) cookie;

	py_args = Py_BuildValue("OziiO", cb_data->py_handle, srvurl, lifetime,
			errcode, cb_data->py_cookie);

	return cb_common(py_args, cookie, 0);
}

/**
 * Callback for the SLPFindSrvTypes() and SLPFindAttrs().
 *
 * RFC 2641 distinguishes between SLPAttrCallback and SLPSrvTypeCallback.
 * However they both have the same argument types and it's up to the python
 * caller to process the data coming from the library anyway.
 *
 * @param hslp 		The language specific SLPHandle on which to register
 * 					the service.
 * @param values	For SLPFindAttrs(): Pointer to a buffer containing a comma
 * 					separated null terminated list of attribute id/value
 * 					assignments in SLP wire format "(attr-id=attr-value-list)".
 * 					For SLPFindSrvTypes(): Pointer to a comma separated list of
 * 					service types.
 * @param errcode 	An error code indicating if an error occurred during
 * 					the operation.
 * @param cookie 	cb_cookie_t storing the python SLP handle, callback function
 * 					and the python callback cookie.
 * @return	SLPBoolean value indicating if the callback wants to process more
 * 			data.
 */
static SLPBoolean srv_attr_type_cb(SLPHandle hslp, const char* values,
		SLPError errcode, void* cookie)
{
	PyObject *py_args;
	cb_cookie_t *cb_data = (cb_cookie_t *) cookie;

	py_args = Py_BuildValue("OziO", cb_data->py_handle, values, errcode,
			cb_data->py_cookie);
	
	return cb_common(py_args, cookie, 0);
}

/**
 * Callback for the SLPReg(), SLPDeReg() and SLPDelAttrs() functions.
 *
 * @param hslp 		The language specific SLPHandle on which to register
 * 					the service.
 * @param errcode 	An error code indicating if an error occurred during
 * 					the operation.
 * @param cookie 	cb_cookie_t storing the python SLP handle, callback function
 * 					and the python callback cookie.
 * @return Nothing.
 */
static void reg_report_cb(SLPHandle hslp, SLPError errcode, void* cookie)
{
	PyObject *py_args;
	cb_cookie_t *cb_data = (cb_cookie_t *) cookie;

	py_args = Py_BuildValue("OiO", cb_data->py_handle, errcode,
			cb_data->py_cookie);
	
	cb_common(py_args, cookie, 1);
}

/**
 * Helper function to extract the SLPHandle and wrap the python objects required
 * for every callback function.
 *
 * @param py_handle		The python object for passing the SLPHandle pointer
 * 						through python code. PyCapsule in fact, opaque for the
 * 						python code.
 * @param py_callback	Python object representing the python function to be
 *						called.
 * @param py_cookie		Arbitrary data to be passed to the python callback.
 * @param ret_hslp		Pointer to the memory where the SLPHandle should be
 * 						"extracted". Inside the py_handle capsule.
 * @param ret_cookie	Newly allocated cb_cookie_t structure pointer. Will hold
 * 						the python handle, callback function and cookie objects.
 * @return	RET_OK (0) on success, RET_ERROR (-1) otherwise.
 */
#define RET_OK 0
#define RET_ERROR -1
static inline int slpfunc_prep_args(PyObject *py_handle, PyObject *py_callback,
		PyObject *py_cookie, SLPHandle *ret_hslp, cb_cookie_t **ret_cookie)
{
	if (!(*ret_hslp = get_slp_handle(py_handle))) {
		PyErr_SetString(PyExc_TypeError, "Invalid SLP handle");
		return RET_ERROR;
	}
	if (!PyCallable_Check(py_callback)) {
		PyErr_SetString(PyExc_TypeError, "Callback must be callable");
		return RET_ERROR;
	}
	if (!(*ret_cookie = malloc(sizeof(cb_cookie_t)))) {
		PyErr_NoMemory();
		return RET_ERROR;
	}
	
	/* Decreased in the callback when no longer needed */
	Py_XINCREF(py_callback);

	(*ret_cookie)->py_handle = py_handle;
	(*ret_cookie)->py_cookie = py_cookie;
	(*ret_cookie)->py_callback = py_callback;

	return RET_OK;
}

/**
 * Helper function for the py_slp_findsrvs() and py_slp_findsttrs -- extracts
 * the arguments.
 *
 * @param args	The python object holding the parameters from the python caller
 *				code.
 * @param hslp	Will hold the SLPHandle to be used with the SLP functions.
 * @param str_arg_1	Where to put the first extracted string
 * @param str_arg_2	Where to put the second extracted string
 * @param str_arg_3	Where to put the third extracted string
 * @param cb_cookie	Where to allocate the cb_cookie_t structure wrapping the
 *					python objects.
 * @return RET_OK (0) on success, RET_ERROR (-1) otherwise.
 */
static int location_func_prep(PyObject *args, SLPHandle *hslp,
		char **str_arg_1, char **str_arg_2, char **str_arg_3,
		cb_cookie_t **cb_cookie)
{
	PyObject *py_handle;
	PyObject *py_callback;
	PyObject *py_cookie;
	
	if (!PyArg_ParseTuple(args, "OzzzOO", &py_handle, str_arg_1, str_arg_2,
				str_arg_3, &py_callback, &py_cookie)) {
		return RET_ERROR;
	}

	return slpfunc_prep_args(py_handle, py_callback, py_cookie,
			hslp, cb_cookie);
}

/**
 * Interface function for SLPOpen().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	Wrapping the SLPOpen arguments:
 * 				lang: String according to RFC 1766, may be None or "".
 * 				isasync: Boolean indicating whether to open for async
 * 				operations.
 * @return	PyObject wrapping a tuple of a SLPHandle wrapper capsule.
 * 			On error returns NULL and raises an exception.
 */
static PyObject *py_slp_open(PyObject *self, PyObject *args)
{
	char *lang;
	SLPBoolean isasync;
	SLPHandle hslp;
	SLPError err;
	PyObject *py_handle;

	if (!PyArg_ParseTuple(args, "zi", &lang, &isasync))
		return NULL;
	if ((err = SLPOpen(lang, isasync, &hslp)) != SLP_OK) {
		PyErr_SetString(PyExc_RuntimeError, get_slp_error_msg(err));
		return NULL;
	}
	if (!(py_handle = PyCapsule_New(hslp, NULL, NULL))) {
		SLPClose(hslp);
		return NULL;
	}
		
	return Py_BuildValue("O", py_handle);
}

/**
 * Interface function for SLPClose().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject capsule storing the SLPHandle pointer.
 * @return	None.
 */
static PyObject *py_slp_close(PyObject *self, PyObject *args)
{
	PyObject *py_handle;
	SLPHandle hslp = NULL;

	if (!PyArg_ParseTuple(args, "O", &py_handle))
		return NULL;
	if ((hslp = get_slp_handle(py_handle))) {
		SLPClose(hslp);
		Py_DECREF(py_handle);
	} else {
		PyErr_SetString(PyExc_TypeError, "The argument to SLPClose doesn't "
				"seem to be a valid SLP handle");
		return NULL;
	}
	
	Py_INCREF(Py_None);
	
	return Py_None;
}

/**
 * Interface function for SLPFindSrvs().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject wrapping the arguments:
 * 				hslp: SLPHandle returned from SLPOpen
 *				srvtype: The service type string -- may be None or ""
 *				scopelist: Comma separated list of scope names -- may be ""
 *				filter: A query formulated of attribute pattern matching
 *				expressions in the form of an LDAPv3 search filter.
 *				callback: te python object representing the callback function to
 *				be called.
 *				cookie: arbitrary data to be passed to the callback.
 * @return	None on success, NULL + exception raised on error.
 */
static PyObject *py_slp_findsrvs(PyObject *self, PyObject *args)
{
	SLPHandle hslp;
	char *srvtype;
	char *scopetype;
	char *filter;
	SLPError err;
	cb_cookie_t *cookie;

	if (location_func_prep(args, &hslp, &srvtype, &scopetype, &filter, &cookie)
			!= RET_OK)
		return NULL;

	if ((err = SLPFindSrvs(hslp, srvtype, scopetype, filter, srv_url_cb,
					(void *)cookie)) != SLP_OK) {
		PyErr_SetString(PyExc_RuntimeError, get_slp_error_msg(err));
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

/**
 * Interface function for SLPFindSrvTypes().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject wrapping the arguments:
 * 				hslp: SLPHandle returned from SLPOpen
 *				namingauth: The naming authorities to search. "*" for all
 *				authorities, "" for the default (IANA)
 *				scopelist: Comma separated list of scope names -- may be ""
 *				callback: te python object representing the callback function to
 *				be called.
 *				cookie: arbitrary data to be passed to the callback.
 * @return	None on success, NULL + exception raised on error.
 */
static PyObject *py_slp_findsrvtypes(PyObject *self, PyObject *args)
{
	SLPHandle hslp;
	PyObject *py_handle;
	PyObject *py_callback;
	PyObject *py_cookie;
	char *namingauth;
	char *scopelist;
	SLPError err;
	cb_cookie_t *cookie;
	
	if (!PyArg_ParseTuple(args, "OzzOO", &py_handle, &namingauth, &scopelist,
				&py_callback, &py_cookie))
		return NULL;
	
	if (slpfunc_prep_args(py_handle, py_callback, py_cookie,
			&hslp, &cookie) != RET_OK)
		return NULL;

	if ((err = SLPFindSrvTypes(hslp, namingauth, scopelist, srv_attr_type_cb,
					(void *)cookie)) != SLP_OK) {
		PyErr_SetString(PyExc_RuntimeError, get_slp_error_msg(err));
		return NULL;
	}

	Py_INCREF(Py_None);
	
	return Py_None;
}

/**
 * Interface function for SLPFindAttrs().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject wrapping the arguments:
 * 				hslp: SLPHandle returned from SLPOpen
 *				srvurl: The service URL of the service to the attributes of.
 *				scopelist: Comma separated list of scope names -- may be ""
 *				attrids: A comma separated list of attribute ids to return.
 *				"" for all attributes.
 *				callback: te python object representing the callback function to
 *				be called.
 *				cookie: arbitrary data to be passed to the callback.
 * @return	None on success, NULL + exception raised on error.
 */
static PyObject *py_slp_findattrs(PyObject *self, PyObject *args)
{
	SLPHandle hslp;
	char *srvurl;
	char *scopelist;
	char *attrids;
	SLPError err;
	cb_cookie_t *cookie;

	if (location_func_prep(args, &hslp, &srvurl, &scopelist, &attrids, &cookie)
			!= RET_OK)
		return NULL;

	if ((err = SLPFindAttrs(hslp, srvurl, scopelist, attrids, srv_attr_type_cb,
					(void *)cookie)) != SLP_OK) {
		PyErr_SetString(PyExc_RuntimeError, get_slp_error_msg(err));
		return NULL;
	}

	Py_INCREF(Py_None);
	
	return Py_None;
}

/**
 * Interface function for SLPReg().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject wrapping the arguments:
 * 				hslp: SLPHandle returned from SLPOpen
 * 				lifetime: An unsigned short giving the lifetime of the service
 * 				in seconds.
 *				srvtype: Ignored, mandated by the RFC 2614, see OpenSLP
 *				documentation for the details.
 *				attrs: Attributes of the registered service. Ex. "(Attr1=val1)"
 *				fresh: True if the registration is new, False for
 *				re-registration
 *				callback: te python object representing the callback function to
 *				be called.
 *				cookie: arbitrary data to be passed to the callback.
 * @return	None on success, NULL + exception raised on error.
 */
static PyObject *py_slp_reg(PyObject *self, PyObject *args)
{
	SLPHandle hslp;
	PyObject *py_handle;
	PyObject *py_callback;
	PyObject *py_cookie;
	PyObject *py_fresh;
	char *srvurl;
	char *srvtype;
	char *attrs;
	unsigned short lifetime;
	SLPBoolean fresh;
	SLPError err;
	cb_cookie_t *cookie;
	
	if (!PyArg_ParseTuple(args, "OsizzOOO", &py_handle, &srvurl, &lifetime,
				&srvtype, &attrs, &py_fresh, &py_callback, &py_cookie))
		return NULL;
	if (slpfunc_prep_args(py_handle, py_callback, py_cookie,
			&hslp, &cookie) != RET_OK)
		return NULL;
	fresh = PyObject_IsTrue(py_fresh);

	if ((err = SLPReg(hslp, srvurl, lifetime, srvtype, attrs, fresh, reg_report_cb,
					(void *)cookie)) != SLP_OK) {
		PyErr_SetString(PyExc_RuntimeError, get_slp_error_msg(err));
		return NULL;
	}
	
	Py_INCREF(Py_None);
	
	return Py_None;
}

/**
 * Interface function for SLPDereg().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject wrapping the arguments:
 * 				hslp: SLPHandle returned from SLPOpen
 *				srvurl: The service URL of the service to be deregistered.
 *				callback: te python object representing the callback function to
 *				be called.
 *				cookie: arbitrary data to be passed to the callback.
 * @return	None on success, NULL + exception raised on error.
 */
static PyObject *py_slp_dereg(PyObject *self, PyObject *args)
{
	SLPHandle hslp;
	PyObject *py_handle;
	PyObject *py_callback;
	PyObject *py_cookie;
	char *srvurl;
	SLPError err;
	cb_cookie_t *cookie;
	
	if (!PyArg_ParseTuple(args, "OsOO", &py_handle, &srvurl, &py_callback,
				&py_cookie))
		return NULL;
	if (slpfunc_prep_args(py_handle, py_callback, py_cookie,
			&hslp, &cookie) != RET_OK)
		return NULL;

	if ((err = SLPDereg(hslp, srvurl, reg_report_cb, (void *)cookie))
			!= SLP_OK) {
		PyErr_SetString(PyExc_RuntimeError, get_slp_error_msg(err));
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

/**
 * Interface function for SLPDelAttrs().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject wrapping the arguments:
 * 				hslp: SLPHandle returned from SLPOpen
 *				srvurl: The service URL of the service to be deregistered.
 *				attrs: Comma separated list of the attributes to be deleted.
 *				callback: te python object representing the callback function to
 *				be called.
 *				cookie: arbitrary data to be passed to the callback.
 * @return	None on success, NULL + exception raised on error.
 */
static PyObject *py_slp_delattrs(PyObject *self, PyObject *args)
{
	SLPHandle hslp;
	PyObject *py_handle;
	PyObject *py_callback;
	PyObject *py_cookie;
	char *srvurl;
	char *attrs;
	SLPError err;
	cb_cookie_t *cookie;
	
	if (!PyArg_ParseTuple(args, "OssOO", &py_handle, &srvurl, &attrs,
				&py_callback, &py_cookie))
		return NULL;
	if (slpfunc_prep_args(py_handle, py_callback, py_cookie,
			&hslp, &cookie) != RET_OK)
		return NULL;

	if ((err = SLPDelAttrs(hslp, srvurl, attrs, reg_report_cb, (void *)cookie))
			!= SLP_OK) {
		PyErr_SetString(PyExc_RuntimeError, get_slp_error_msg(err));
		return NULL;
	}
	
	Py_INCREF(Py_None);
	
	return Py_None;
}

/**
 * Interface function for SLPGetRefreshInterval().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	Unused.
 * @return	A positive integer value that is the smallest value that should be
 *			passed as a lifetime to SLPReg().
 */
static PyObject *py_slp_get_refresh_interval(PyObject *self, PyObject *args)
{
	return Py_BuildValue("i", SLPGetRefreshInterval());
}

/**
 * Interface function for SLPFindScopes().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject wrapping the arguments:
 * 				hslp: SLPHandle returned from SLPOpen
 * @return	Comma separated list of the available scopes.
 */
static PyObject *py_slp_find_scopes(PyObject *self, PyObject *args)
{
	SLPHandle hslp;
	SLPError err;
	PyObject *py_handle;
	PyObject *ret;
	char *scopelist;

	if (!PyArg_ParseTuple(args, "O", &py_handle))
		return NULL;
	if (!(hslp = get_slp_handle(py_handle))) {
		PyErr_SetString(PyExc_TypeError, "The argument doesn't "
				"seem to be a valid SLP handle");
		return NULL;
	}

	if ((err = SLPFindScopes(hslp, &scopelist)) != SLP_OK) {
		PyErr_SetString(PyExc_RuntimeError, get_slp_error_msg(err));
		return NULL;
	}
	
	/* There should be always at least the "DEFAULT" scope. */
	ret = Py_BuildValue("s", scopelist);
	SLPFree(scopelist);

	return ret;
}

/**
 * Interface function for SLPGetProperty().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject wrapping the arguments:
 * 				name: The name of a net.slp property to get.
 * @return	The string with the property value. May be None.
 */
static PyObject *py_slp_get_property(PyObject *self, PyObject *args)
{
	char *name;
	const char *val;

	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;
	if ((val = SLPGetProperty(name))) {
		return Py_BuildValue("s", val);
	} else {
		Py_INCREF(Py_None);
		return Py_None;
	}
}

/**
 * Interface function for SLPSetProperty().
 *
 * Please note: this function does nothing. Consult the OpenSLP documentation
 * for more details why.
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject wrapping the arguments:
 * 				name: The name of a net.slp property to set.
 * 				value: The new value.
 * @return	The string with the property value. May be None.
 */
static PyObject *py_slp_set_property(PyObject *self, PyObject *args)
{
	char *name;
	char *value;

	if (!PyArg_ParseTuple(args, "zz", &name, &value))
		return NULL;

	/* No-op */
	SLPSetProperty(name, value);

	Py_INCREF(Py_None);

	return Py_None;
}

/**
 * Interface function for SLPParseSrvURL().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject wrapping the arguments:
 * 				srvurl: The URL string to be parsed.
 * @return	The tuple consisting of the SLPSrvURL structure members.
 */
static PyObject *py_slp_parse_srvurl(PyObject *self, PyObject *args)
{
	char *srvurl;
	SLPSrvURL *parsedurl = NULL;
	PyObject *ret;
	SLPError err;
	
	if (!PyArg_ParseTuple(args, "z", &srvurl))
		return NULL;
	
	if((err = SLPParseSrvURL(srvurl, &parsedurl)) != SLP_OK) {
		PyErr_SetString(PyExc_RuntimeError, get_slp_error_msg(err));
		return NULL;
	}

	ret = Py_BuildValue("zzizz",
			parsedurl->s_pcSrvType,
			parsedurl->s_pcHost,
			parsedurl->s_iPort,
			parsedurl->s_pcNetFamily,
			parsedurl->s_pcSrvPart);
	SLPFree(parsedurl);

	return ret;
}

/**
 * Interface function for SLPEscape().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject wrapping the arguments:
 * 				unescaped: The string to be escaped.
 * 				istag: Check for bad characters.
 * @return	The escaped string.
 */
static PyObject *py_slp_escape(PyObject *self, PyObject *args)
{
	char *unescaped;
	char *escaped;
	PyObject *ret;
	PyObject *py_istag;
	SLPBoolean istag;
	SLPError err;
	
	if (!PyArg_ParseTuple(args, "si", &unescaped, &py_istag))
		return NULL;
	
	istag = PyObject_IsTrue(py_istag);

	if((err = SLPEscape(unescaped, &escaped, istag)) != SLP_OK) {
		PyErr_SetString(PyExc_RuntimeError, get_slp_error_msg(err));
		return NULL;
	}

	ret = Py_BuildValue("z", escaped);
	SLPFree(escaped);

	return ret;
}

/**
 * Interface function for SLPUnescape().
 *
 * @param self	Unused. Mandated by the Python C API.
 * @param args	PyObject wrapping the arguments:
 * 				escaped: The string to be un-escaped.
 * 				istag: Check for bad characters.
 * @return	The un-escaped string.
 */
static PyObject *py_slp_unescape(PyObject *self, PyObject *args)
{
	char *unescaped;
	char *escaped;
	PyObject *ret;
	PyObject *py_istag;
	SLPBoolean istag;
	SLPError err;
	
	if (!PyArg_ParseTuple(args, "si", &escaped, &py_istag))
		return NULL;
	
	istag = PyObject_IsTrue(py_istag);

	if((err = SLPUnescape(escaped, &unescaped, istag)) != SLP_OK) {
		PyErr_SetString(PyExc_RuntimeError, get_slp_error_msg(err));
		return NULL;
	}

	ret = Py_BuildValue("z", unescaped);
	SLPFree(unescaped);

	return ret;
}

/* The methods table. TODO: Add the Python description strings. */
static PyMethodDef slp_methods[] = {
	/* handle functions */
	{ "SLPOpen", py_slp_open, METH_VARARGS, NULL },
	{ "SLPClose", py_slp_close, METH_VARARGS, NULL },
	/* service location functions */
	{ "SLPFindSrvs", py_slp_findsrvs, METH_VARARGS, NULL },
	{ "SLPFindSrvTypes", py_slp_findsrvtypes, METH_VARARGS, NULL },
	{ "SLPFindAttrs", py_slp_findattrs, METH_VARARGS, NULL },
	/* service registration functions */
	{ "SLPReg", py_slp_reg, METH_VARARGS, NULL },
	{ "SLPDereg", py_slp_dereg, METH_VARARGS, NULL },
	{ "SLPDelAttrs", py_slp_delattrs, METH_VARARGS, NULL },
	/* configuration functions */
	{ "SLPGetRefreshInterval", py_slp_get_refresh_interval,
		METH_VARARGS, NULL },
	{ "SLPFindScopes", py_slp_find_scopes, METH_VARARGS, NULL },
	{ "SLPGetProperty", py_slp_get_property, METH_VARARGS, NULL },
	{ "SLPSetProperty", py_slp_set_property, METH_VARARGS, NULL },
	/* parsing functions */
	{ "SLPParseSrvURL", py_slp_parse_srvurl, METH_VARARGS, NULL },
	{ "SLPEscape", py_slp_escape, METH_VARARGS, NULL },
	{ "SLPUnescape", py_slp_unescape, METH_VARARGS, NULL },
	/* SLPFree() not implemented. */
	{ NULL, NULL, 0, NULL }
};

# define ADD_INT_VAR(__m, __n, __v) \
	do { \
		PyObject *__o = PyInt_FromLong((long)(__v)); \
		PyObject_SetAttrString(__m, __n, __o); \
		Py_DECREF(__o); \
	} while (0)

#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef slpmodule = {
	PyModuleDef_HEAD_INIT,
	"slp",
	NULL,
	-1,
	slp_methods
};
#endif

#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC PyInit_slp(void)
{
	PyObject *m;

	m = PyModule_Create(&slpmodule);
#else
PyMODINIT_FUNC initslp(void)
{
	PyObject *m;

	m = Py_InitModule("slp", slp_methods);
#endif
	/* Now add some named variables */
	ADD_INT_VAR(m, "SLP_LIFETIME_MAXIMUM", SLP_LIFETIME_MAXIMUM);
	ADD_INT_VAR(m, "SLP_LIFETIME_DEFAULT", SLP_LIFETIME_DEFAULT);
	ADD_INT_VAR(m, "SLP_OK", SLP_OK);
	ADD_INT_VAR(m, "SLP_LANGUAGE_NOT_SUPPORTED", SLP_LANGUAGE_NOT_SUPPORTED);
	ADD_INT_VAR(m, "SLP_PARSE_ERROR", SLP_PARSE_ERROR);
	ADD_INT_VAR(m, "SLP_INVALID_REGISTRATION", SLP_INVALID_REGISTRATION);
	ADD_INT_VAR(m, "SLP_SCOPE_NOT_SUPPORTED", SLP_SCOPE_NOT_SUPPORTED);
	ADD_INT_VAR(m, "SLP_AUTHENTICATION_ABSENT", SLP_AUTHENTICATION_ABSENT);
	ADD_INT_VAR(m, "SLP_AUTHENTICATION_FAILED", SLP_AUTHENTICATION_FAILED);
	ADD_INT_VAR(m, "SLP_INVALID_UPDATE", SLP_INVALID_UPDATE);
	ADD_INT_VAR(m, "SLP_REFRESH_REJECTED", SLP_REFRESH_REJECTED);
	ADD_INT_VAR(m, "SLP_NOT_IMPLEMENTED", SLP_NOT_IMPLEMENTED);
	ADD_INT_VAR(m, "SLP_BUFFER_OVERFLOW", SLP_BUFFER_OVERFLOW);
	ADD_INT_VAR(m, "SLP_NETWORK_TIMED_OUT", SLP_NETWORK_TIMED_OUT);
	ADD_INT_VAR(m, "SLP_NETWORK_INIT_FAILED", SLP_NETWORK_INIT_FAILED);
	ADD_INT_VAR(m, "SLP_MEMORY_ALLOC_FAILED", SLP_MEMORY_ALLOC_FAILED);
	ADD_INT_VAR(m, "SLP_PARAMETER_BAD", SLP_PARAMETER_BAD);
	ADD_INT_VAR(m, "SLP_NETWORK_ERROR", SLP_NETWORK_ERROR);
	ADD_INT_VAR(m, "SLP_INTERNAL_SYSTEM_ERROR", SLP_INTERNAL_SYSTEM_ERROR);
	ADD_INT_VAR(m, "SLP_HANDLE_IN_USE", SLP_HANDLE_IN_USE);
	ADD_INT_VAR(m, "SLP_TYPE_ERROR", SLP_TYPE_ERROR);
	ADD_INT_VAR(m, "SLP_LAST_CALL", SLP_LAST_CALL);

	return m;
}

