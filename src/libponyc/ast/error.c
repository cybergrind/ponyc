#include "error.h"
#include "stringtab.h"
#include "../../libponyrt/mem/pool.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define LINE_LEN 1024

static errormsg_t* head = NULL;
static errormsg_t* tail = NULL;
static size_t count = 0;
static bool immediate_report = false;


static void print_errormsg(errormsg_t* e, const char* indent)
{
  if(e->file != NULL)
  {
    printf("%s%s:", indent, e->file);

    if(e->line != 0)
    {
      printf(__zu ":" __zu ": ", e->line, e->pos);
    }
    else {
      printf(" ");
    }
  }

  printf("%s\n", e->msg);

  if(e->source != NULL)
  {
    printf("%s%s\n", indent, e->source);
    printf("%s", indent);

    for(size_t i = 0; i < (e->pos - 1); i++)
    {
      if(e->source[i] == '\t')
        printf("\t");
      else
        printf(" ");
    }

    printf("^\n");
  }
}

static void print_error(errormsg_t* e)
{
  printf("Error:\n");
  print_errormsg(e, "");

  if(e->frame != NULL)
  {
    printf("    Info:\n");

    for(errormsg_t* p = e->frame; p != NULL; p = p->frame)
      print_errormsg(p, "    ");
  }
}

static void add_error(errormsg_t* e)
{
  if(immediate_report)
    print_error(e);

  if(head == NULL)
  {
    head = e;
    tail = e;
  } else {
    tail->next = e;
    tail = e;
  }

  e->next = NULL;
  count++;
}

static void append_to_frame(errorframe_t* frame, errormsg_t* e)
{
  assert(frame != NULL);

  if(*frame == NULL)
  {
    *frame = e;
  }
  else
  {
    errormsg_t* p = *frame;
    while(p->frame != NULL)
      p = p->frame;

    p->frame = e;
  }
}

errormsg_t* get_errors()
{
  return head;
}

size_t get_error_count()
{
  return count;
}

static void free_error(errormsg_t* e)
{
  while(e != NULL)
  {
    errormsg_t* next = e->frame;
    POOL_FREE(errormsg_t, e);
    e = next;
  }
}

void free_errors()
{
  errormsg_t* e = head;
  head = NULL;
  tail = NULL;

  while(e != NULL)
  {
    errormsg_t* next = e->next;
    free_error(e);
    e = next;
  }

  count = 0;
}

void print_errors()
{
  if(immediate_report)
    return;

  errormsg_t* e = head;

  while(e != NULL)
  {
    print_error(e);
    e = e->next;
  }
}

static errormsg_t* make_errorv(source_t* source, size_t line, size_t pos,
  const char* fmt, va_list ap)
{
  char buf[LINE_LEN];
  vsnprintf(buf, LINE_LEN, fmt, ap);

  errormsg_t* e = POOL_ALLOC(errormsg_t);
  memset(e, 0, sizeof(errormsg_t));

  if(source != NULL)
    e->file = source->file;

  e->line = line;
  e->pos = pos;
  e->msg = stringtab(buf);

  if((source != NULL) && (line != 0))
  {
    size_t tline = 1;
    size_t tpos = 0;

    while((tline < e->line) && (tpos < source->len))
    {
      if(source->m[tpos] == '\n')
        tline++;

      tpos++;
    }

    size_t start = tpos;

    while((source->m[tpos] != '\n') && (tpos < source->len))
      tpos++;

    size_t len = tpos - start;

    if(len >= sizeof(buf))
      len = sizeof(buf) - 1;

    memcpy(buf, &source->m[start], len);
    buf[len] = '\0';
    e->source = stringtab(buf);
  }

  return e;
}

void errorv(source_t* source, size_t line, size_t pos, const char* fmt,
  va_list ap)
{
  add_error(make_errorv(source, line, pos, fmt, ap));
}

void error(source_t* source, size_t line, size_t pos, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  errorv(source, line, pos, fmt, ap);
  va_end(ap);
}

void errorframev(errorframe_t* frame, source_t* source, size_t line,
  size_t pos, const char* fmt, va_list ap)
{
  assert(frame != NULL);
  append_to_frame(frame, make_errorv(source, line, pos, fmt, ap));
}

void errorframe(errorframe_t* frame, source_t* source, size_t line, size_t pos,
  const char* fmt, ...)
{
  assert(frame != NULL);

  va_list ap;
  va_start(ap, fmt);
  errorframev(frame, source, line, pos, fmt, ap);
  va_end(ap);
}

static errormsg_t* make_errorfv(const char* file, const char* fmt, va_list ap)
{
  char buf[LINE_LEN];
  vsnprintf(buf, LINE_LEN, fmt, ap);

  errormsg_t* e = POOL_ALLOC(errormsg_t);
  memset(e, 0, sizeof(errormsg_t));

  e->file = stringtab(file);
  e->msg = stringtab(buf);
  return e;
}

void errorfv(const char* file, const char* fmt, va_list ap)
{
  add_error(make_errorfv(file, fmt, ap));
}

void errorf(const char* file, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  errorfv(file, fmt, ap);
  va_end(ap);
}

void errorframefv(errorframe_t* frame, const char* file, const char* fmt,
  va_list ap)
{
  assert(frame != NULL);
  append_to_frame(frame, make_errorfv(file, fmt, ap));
}

void errorframef(errorframe_t* frame, const char* file, const char* fmt, ...)
{
  assert(frame != NULL);

  va_list ap;
  va_start(ap, fmt);
  errorframefv(frame, file, fmt, ap);
  va_end(ap);
}

void errorframe_append(errorframe_t* first, errorframe_t* second)
{
  assert(first != NULL);
  assert(second != NULL);

  append_to_frame(first, *second);
  *second = NULL;
}

bool errorframe_has_errors(errorframe_t* frame)
{
  assert(frame != NULL);
  return frame != NULL;
}

void errorframe_report(errorframe_t* frame)
{
  assert(frame != NULL);

  if(*frame != NULL)
    add_error(*frame);

  *frame = NULL;
}

void errorframe_discard(errorframe_t* frame)
{
  assert(frame != NULL);

  free_error(*frame);
  *frame = NULL;
}

void error_set_immediate(bool immediate)
{
  immediate_report = immediate;
}
