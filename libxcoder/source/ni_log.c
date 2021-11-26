/*!****************************************************************************
*
* Copyright (C)  2018 NETINT Technologies. 
*
* Permission to use, copy, modify, and/or distribute this software for any 
* purpose with or without fee is hereby granted.
*
*******************************************************************************/

/*!*****************************************************************************
*  \file   ni_log.c
*
*  \brief  Exported logging routines definition
*
*******************************************************************************/

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#ifdef __linux__
#include <sys/time.h>
#include <inttypes.h>
#endif
#include "ni_log.h"

static ni_log_level_t ni_log_level = NI_LOG_INFO;
static void (*ni_log_callback)(int, const char*, va_list) = ni_log_default_callback;

/*!******************************************************************************
 *  \brief Default ni_log() callback
 *
 *  \param[in] level  log level
 *  \param[in] fmt    printf format specifier
 *  \param[in] vl     variadric args list
 *
 *  \return
 *******************************************************************************/
void ni_log_default_callback(int level, const char* fmt, va_list vl)
{
  if (level <= ni_log_level)
  {
# if defined(NI_LOG_TRACE_TIMESTAMPS) && defined(__linux__)
    if (level == NI_LOG_TRACE)
    {
      struct timeval tv;
      gettimeofday(&tv, NULL);
      fprintf(stderr, "[%" PRIu64 "] ", tv.tv_sec * 1000000LL + tv.tv_usec);
    }
# endif
    vfprintf(stderr, fmt, vl);
  }
}

/*!******************************************************************************
 *  \brief  Set ni_log() callback
 *
 *  \param[in] callback
 *
 *  \return
 *******************************************************************************/
void ni_log_set_callback(void (*log_callback)(int, const char*, va_list))
{
  ni_log_callback = log_callback;
}

/*!******************************************************************************
 *  \brief  print log message using ni_log_callback
 *
 *  \param[in] level  log level
 *  \param[in] format printf format specifier
 *  \param[in] ...    additional arguments
 *
 *  \return
 *******************************************************************************/
void ni_log(ni_log_level_t level, const char *fmt, ...)
{
  va_list vl;
  void (*log_callback)(int, const char*, va_list);

  va_start(vl, fmt);
  if (log_callback = ni_log_callback)
    log_callback(level, fmt, vl);
  va_end(vl);
}

/*!******************************************************************************
 *  \brief  Set ni_log_level
 *
 *  \param  level log level
 *
 *  \return
 *******************************************************************************/
void ni_log_set_level(ni_log_level_t level)
{
  ni_log_level = level;
}

/*!******************************************************************************
 *  \brief Get ni_log_level
 *
 *  \return ni_log_level
 *******************************************************************************/
ni_log_level_t ni_log_get_level(void)
{
  return ni_log_level;
}

/*!******************************************************************************
 *  \brief Convert ffmpeg log level integer to appropriate ni_log_level_t
 *
 *  \param fflog_level integer representation of FFmpeg log level
 *
 *  \return ni_log_level
 *******************************************************************************/
ni_log_level_t ff_to_ni_log_level(int fflog_level)
{
  ni_log_level_t converted_ni_log_level = NI_LOG_ERROR;
  if (fflog_level >= -8)
  {
    converted_ni_log_level = NI_LOG_NONE;
  }
  if (fflog_level >= 8)
  {
    converted_ni_log_level = NI_LOG_FATAL;
  }
  if (fflog_level >= 16)
  {
    converted_ni_log_level = NI_LOG_ERROR;
  }
  if (fflog_level >= 32)
  {
    converted_ni_log_level = NI_LOG_INFO;
  }
  if (fflog_level >= 48)
  {
    converted_ni_log_level = NI_LOG_DEBUG;
  }
  if (fflog_level >= 56)
  {
    converted_ni_log_level = NI_LOG_TRACE;
  }
  return converted_ni_log_level;
}
