/*
 *	$Source: /afs/dev.mit.edu/source/repository/athena/bin/discuss/server/acl_core.c,v $
 *	$Header: /afs/dev.mit.edu/source/repository/athena/bin/discuss/server/acl_core.c,v 1.1 1986-11-16 06:03:56 wesommer Exp $
 *
 *	Copyright (C) 1986 by the Massachusetts Institute of Technology
 *
 *	Routines for use in a server to edit access control lists remotely.
 *	Originally written for the discuss system by Bill Sommerfeld
 *
 *	$Log: not supported by cvs2svn $
 */

#include "../include/types.h"
#include "../include/dsc_et.h"
#include "../include/acl.h"
#include <sys/file.h>
#include <errno.h>
#include <stdio.h>
#include <sys/param.h>

#ifndef lint
static char *rcsid_acl_core_c = "$Header: /afs/dev.mit.edu/source/repository/athena/bin/discuss/server/acl_core.c,v 1.1 1986-11-16 06:03:56 wesommer Exp $";
#endif lint

extern Acl *mtg_acl;
extern char rpc_caller [];
extern int errno;

/*
 * Routines provided:
 *	get_acl(obj_name) returns(code, acl)
 *		Return the access control list for an object (meeting).
 *      get_access(obj_name, princ_name) returns(code, modes)
 *		Returns the access allowed for a given principal
 *	set_access(obj_name, princ_name, modes) returns(code)
 *		Sets the access on an object for a specific principal.
 *	delete_access(obj_name, princ_name) returns(code)
 *		Deletes princ_name from the access control list; 
 */

/* 
 *	Locking protocol: 
 *	It is assumed that the ACL is in a file in a known directory; 
 *	the following locking protocol is used when modifying ACL's
 *	to ensure consistancy.
 *	1) flock EXCLUSIVE parent directory.
 *	2) open acl for read, read ACL.
 *	3) modify ACL in core.
 *	4) open #acl for write, write ACL.
 *	5) rename #acl to acl (this is assumed to be atomic)
 *	6) unlock parent directory.
 *
 *	The directory is locked, rather than the acl, because the file is
 *	recreated and deleted (for atomicity reasons) on every transaction
 *	(thank god UNIX files are lightweight).  
 *
 *	Since the acl in place is always consistant (and file opens 
 *	are atomic), there is no need to read-lock when getting the ACL.
 */

get_acl(mtg_name, code, list)
	char *mtg_name;
	int *code;		/* RETURN */
	Acl **list;		/* RETURN */
{

	/* 
	 * bug with current code:  open_mtg should check that acl has not 
	 * been changed since it was sucked into memory; a simple stat() 
	 * call should suffice.  
	 */

	*list = NULL;		/* bogon ACL */
	if((*code = open_mtg(mtg_name)) != 0) {
		return;
	}

	if(!has_mtg_access('s')) {
		*code = NO_ACCESS;
		return;
	}

	*code = 0;
	*list = mtg_acl;
	return;
}

get_access(mtg_name, princ_name, modes, code)
	char *mtg_name;
	char *princ_name;
	char **modes;		/* RETURN */
	int *code;		/* RETURN */
{
	*modes = NULL;
		
	if ((*code = open_mtg(mtg_name)) != 0) {
		return;
	}

	if(!has_mtg_access('s')) {
		*code = NO_ACCESS;
		return;
	}

	*code = 0;
	*modes = acl_get_access(mtg_acl, princ_name);
	return;
}

/*
 * Set access.
 */

set_access(mtg_name, princ_name, modes, code)
	char *mtg_name;
	char *princ_name;
	char *modes;
	int *code;		/* RETURN */
{
	int lockfd;
	char acl_name[MAXPATHLEN];
	Acl *acl;
	char *n_modes;
	/*
	 * We do not use open_mtg here; it does more than what we need.
	 */

	n_modes = acl_canon(modes, "acdorsw", code);
	if (*code) goto punt;

	/* steps 1 and 2 performed by locked_open_mtg */
	if ((*code = locked_open_mtg(mtg_name, &lockfd, acl_name, &acl))
	    != 0) {
		goto punt;
	}

	/* step 3: modify ACL in core */
	if (!acl_check(acl,rpc_caller,"c")) {
		*code = NO_ACCESS;
		locked_abort(mtg_name, lockfd, acl_name, acl);
		goto punt;
	}

	acl_replace_access(acl, princ_name, n_modes);
			    
	/* step 4, 5, 6 performed by locked_close_mtg */
	if((*code = locked_close_mtg(mtg_name, lockfd, acl_name, acl))
	   != 0) {
		   goto punt;
	}
	*code = 0;
 punt:
	free(n_modes);
	return;
}

/*
 * Delete access.  This should look a lot like the above..
 */

delete_access(mtg_name, princ_name, code)
	char *mtg_name;
	char *princ_name;
	int *code;		/* RETURN */
{
	int lockfd;
	char acl_name[MAXPATHLEN];
	Acl *acl;

	/*
	 * We do not use open_mtg here; it does more than what we need.
	 */

	/* steps 1 and 2 performed by locked_open_mtg */
	if ((*code = locked_open_mtg(mtg_name, &lockfd, acl_name, &acl))
	    != 0) {
		return;
	}

	/* step 3: modify ACL in core */
	if (!acl_check(acl,rpc_caller,"c")) {
		*code = NO_ACCESS;
		locked_abort(mtg_name, lockfd, acl_name, acl);
		return;
	}

	if(!acl_delete_access(acl, princ_name)) {
		*code = NO_PRINC;
		locked_abort(mtg_name, lockfd, acl_name, acl);
		return;
	}
	
	/* step 4, 5, 6 performed by locked_close_mtg */
	if((*code = locked_close_mtg(mtg_name, lockfd, acl_name, acl))
	   != 0) {
		return;
	}
	*code = 0;
	return;
}

int
locked_open_mtg(mtg_name, lockfd, acl_name, acl)
	char *mtg_name;
	int *lockfd;		/* RETURN */
	char *acl_name;		/* RETURN - address of buffer */
	Acl **acl;		/* RETURN */
{
	int mtg_name_len = strlen (mtg_name);
	int result;
	int u_acl_f;
	
	*lockfd = -1;
	u_acl_f = -1;
	/* XXX historical magic number should be MAXPATHLEN */
	if (mtg_name[0] != '/' || mtg_name_len == 0 || 
	    mtg_name_len > 168 || mtg_name [mtg_name_len-1] == '/') {
		result = BAD_PATH;
		goto punt;
	}
	if((*lockfd = open(mtg_name, O_RDONLY, 0700)) < 0) {
		result = errno;
		goto punt;
	}
	if((flock(*lockfd, LOCK_EX)) != 0) { /* may block... */
		result = errno;
		goto punt;
	}
	strcpy (acl_name, mtg_name);
	strcat (acl_name, "/acl");

	if ((u_acl_f = open(acl_name, O_RDONLY, 0700)) < 0) {
		if (errno == ENOENT)
			result = NO_SUCH_MTG;
		else if (errno == EACCES)
			result = NO_ACCESS;
		else
			result = BAD_PATH;
		goto punt;
	}
	if (!fis_owner (u_acl_f, geteuid())) {
		result = NO_ACCESS;
		goto punt;
	}

	*acl = acl_read (u_acl_f);
	close(u_acl_f);
	u_acl_f = 0;
	return 0;
punt:
	if (*lockfd >= 0) close(*lockfd);
	if (u_acl_f >= 0) close(u_acl_f);
	if (*acl) acl_destroy(*acl);

	return result;
}

locked_abort(mtg_name, lockfd, acl_name, acl)
	char *mtg_name;
	int lockfd;
	char *acl_name;
	Acl *acl;
{
	close(lockfd);		/* unlocks, too */
	acl_destroy(acl);
}

/*
 * Here's the hairy one.
 */

int
locked_close_mtg(mtg_name, lockfd, acl_name, acl)
	char *mtg_name;
	int lockfd;
	char *acl_name;
	Acl *acl;
{
	int u_acl_f = -1;
	int result;
	char acl_nname[MAXPATHLEN];

	/*
	 * 4: Open up acl file for writing and write new acl.
	 */
	strcpy(acl_nname, acl_name);
	strcat(acl_nname, "~");

	if ((u_acl_f = open(acl_nname, O_WRONLY|O_TRUNC|O_CREAT, 0600)) < 0) {
		result = errno; /*XXX*/
		goto punt;
	}

	if(!acl_write(u_acl_f, acl)) {
		result = errno; /*XXX*/
		goto punt;
	}

	close(u_acl_f); u_acl_f = -1;
	
	/*
	 * 5: the commit point; rename the file.
	 */
	
	if(rename(acl_nname, acl_name) != 0)  {
		result = errno;
		goto punt;
	}
	/*
	 * 6: Cleanup.
	 */

	result = 0;
 punt:
	close(lockfd);	 	/* unlock */
	acl_destroy(acl); 	/* kaboom */
	if (u_acl_f >= 0) close(u_acl_f);
	return result;
}