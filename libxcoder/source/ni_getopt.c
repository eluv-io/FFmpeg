/*!****************************************************************************
*
* Copyright (C)  2018 NETINT Technologies.
*
* Permission to use, copy, modify, and/or distribute this software for any
* purpose with or without fee is hereby granted.
*
*******************************************************************************/

/*!*****************************************************************************
* \file   ni_getopt.c
*
* \brief  Implementation of getopt and getopt_long with Windows API.
*
*******************************************************************************/

#include <stdio.h>
#include "ni_getopt.h"

TCHAR	*optarg = NULL; // global argument pointer
int		optind = 0; 	// global argv index

int getopt(int argc, TCHAR *argv[], const TCHAR *optstring)
{
	static TCHAR *nextchar = NULL;

	if (nextchar == NULL || nextchar == _T('\0'))
	{
		if (optind == 0)
			optind++;

		if (optind >= argc || argv[optind][0] != _T('-') || argv[optind][1] == _T('\0'))
		{
			optarg = NULL;
			if (optind < argc)
				optarg = argv[optind];
			return EOF;
		}

		if (_tcsncmp(argv[optind], _T("--"), _tcslen(_T("--"))) == 0)
		{
			optind++;
			optarg = NULL;
			if (optind < argc)
				optarg = argv[optind];
			return EOF;
		}

		nextchar = argv[optind];
		nextchar += _tcslen(_T("-"));
		optind++;
	}

	TCHAR c = *nextchar++;
	TCHAR *cp = _tcschr(optstring, c);

	if (cp == NULL || c == _T(':'))
		return _T('?');

	cp++;
	if (*cp == _T(':'))
	{
		if (*nextchar != _T('\0'))
		{
			optarg = nextchar;
			nextchar = NULL;  // for next invocation
		}
		else if (optind < argc)
		{
			optarg = argv[optind];
			optind++;
		}
		else
		{
			return _T('?');
		}
	}

	return c;
}


int getopt_long(int argc, TCHAR* argv[], const TCHAR* optstring,
	            const struct option* longopts, int* longindex)
{
	int i;
	static TCHAR* nextchar = NULL;

	if (nextchar == NULL || *nextchar == _T('\0'))
	{
		if (optind == 0)
		{
			optind++;
		}

		if (optind >= argc || argv[optind][0] != _T('-') || argv[optind][1] == _T('\0'))
		{
			optarg = NULL;
			if (optind < argc)
				optarg = argv[optind];
			return EOF;
		}

		nextchar = argv[optind];
		if (_tcsncmp(argv[optind], _T("--"), 2) == 0)
		{
			nextchar += _tcslen(_T("--"));
			optarg = NULL;
			if (optind < argc)
				optarg = argv[optind];
		}
		else
		{
			nextchar += _tcslen(_T("-"));
		}

		optind++;
	}

	// Parse long option string
	for (i = 0; longopts != NULL && longopts[i].name != NULL; i++)
	{
		size_t optlen = _tcslen(_T(longopts[i].name));
		if (_tcsncmp(nextchar, _T(longopts[i].name), optlen) == 0)
		{
			optarg = nextchar + optlen;
			switch (longopts[i].has_arg)
			{
			case 0:
				if (*optarg != _T('\0') || argv[optind][0] != _T('-'))
				{
					return _T('?');
				}
				else
				{
					return longopts[i].flag == NULL ? longopts[i].val : 0;
				}
			case 1:
				if (*optarg == _T('='))
				{
					optarg += _tcslen(_T("="));
					return longopts[i].flag == NULL ? longopts[i].val : 0;
				}
				else if (*optarg != _T('\0') || argv[optind][0] == _T('-'))
				{
					return _T('?');
				}
				else
				{
					optarg = argv[optind];
					return longopts[i].flag == NULL ? longopts[i].val : 0;
				}
			case 2:
				if (*optarg == _T('\0') || argv[optind][0] == _T('-'))
				{
					optarg = NULL;
				}
				else
				{
					if (*optarg == _T('\0'))
					{
						optarg = argv[optind];
					}
				}
				return longopts[i].flag == NULL ? longopts[i].val : 0;
			default:
				return _T('?');
			}
		}
	}

	// Parse short option string
	TCHAR c = *nextchar++;
	TCHAR* cp = _tcschr(optstring, c);

	if (cp == NULL || c == _T(':'))
	{
		return _T('?');
	}

	cp++;
	// Whether there is any argument
	if (*cp == _T(':'))
	{
		if (*nextchar != _T('\0'))
		{
			optarg = nextchar;
			nextchar = NULL;  // for next invocation
		}
		else if (optind < argc)
		{
			optarg = argv[optind];
			optind++;
		}
		else
		{
			return _T('?');
		}
	}

	return c;
}