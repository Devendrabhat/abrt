%{ /* -*- mode: yacc -*-
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "backtrace.h"
#include "strbuf.h"

struct backtrace *g_backtrace;

#define YYDEBUG 1
#define YYMAXDEPTH 10000000
void yyerror(char const *s)
{
  fprintf (stderr, "\nParser error: %s\n", s);
}

int yylex();

%}

/* This defines the type of yylval */
%union {
  struct backtrace *backtrace;
  struct thread *thread;
  struct frame *frame;
  char *str;
  int num;
  char c;

  struct strbuf *strbuf;
}

/* Bison declarations.  */
%token END 0 "end of file"

%type <backtrace> backtrace
%type <thread> threads
               thread
%type <frame>  frames
               frame
               frame_head
               frame_head_1
               frame_head_2
               frame_head_3
               frame_head_4
               frame_head_5
%type <strbuf> identifier
               hexadecimal_digit_sequence
               hexadecimal_number
               file_name
               file_location
               function_call
               function_name
               digit_sequence
               frame_address_in_function
               identifier_braces
               identifier_braces_inside
               identifier_template
               identifier_template_inside
%type <c>      nondigit
               digit
               hexadecimal_digit
               file_name_char
               identifier_char
               identifier_char_no_templates
               identifier_first_char
               identifier_braces_inside_char
               identifier_template_inside_char
               variables_char
               variables_char_no_framestart
               ws
               ws_nonl
               '(' ')' '+' '-' '/' '.' '_' '~' '[' ']' '\r' '?' '{' '}'
               'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l'
               'm' 'n' 'o' 'p' 'q' 'r' 's' 't' 'u' 'v' 'w' 'x' 'y' 'z'
               'A' 'B' 'C' 'D' 'E' 'F' 'G' 'H' 'I' 'J' 'K' 'L' 'M' 'N'
               'O' 'P' 'Q' 'R' 'S' 'T' 'U' 'V' 'W' 'X' 'Y' 'Z'
               '0' '1' '2' '3' '4' '5' '6' '7' '8' '9'
               '\'' '`' ',' '#' '@' '<' '>' '=' ':' '"' ';' ' '
               '\n' '\t' '\\' '!' '*' '%' '|' '^' '&' '$'
%type <num>    frame_start

/*
%destructor { thread_free($$); } <thread>
%destructor { frame_free($$); } <frame>
%destructor { strbuf_free($$); } <strbuf>
*/

%start backtrace
%glr-parser
%error-verbose
%locations

%% /* The grammar follows.  */

backtrace : /* empty */ %dprec 1
             { $$ = g_backtrace = backtrace_new(); }
          | threads wsa %dprec 2
             {
               $$ = g_backtrace = backtrace_new();
               $$->threads = $1;
             }
          |  frame_head wss threads wsa %dprec 4
             {
               $$ = g_backtrace = backtrace_new();
               $$->threads = $3;
               $$->crash = $1;
             }
          | frame wss threads wsa %dprec 3
             {
               $$ = g_backtrace = backtrace_new();
               $$->threads = $3;
               $$->crash = $1;
             }
;

threads   : thread
          | threads '\n' thread { $$ = thread_add_sibling($1, $3); }
;

thread    : keyword_thread wss digit_sequence wsa '(' keyword_thread wss digit_sequence wsa ')' ':' wsa frames
              {
                $$ = thread_new();
                $$->frames = $13;

		if (sscanf($3->buf, "%d", &$$->number) != 1)
		{
		  printf("Error while parsing thread number '%s'", $3->buf);
		  exit(5);
		}
		strbuf_free($3);
		strbuf_free($8);
              }
;

frames    : frame        { $$ = $1; }
          | frames frame { $$ = frame_add_sibling($1, $2); }
;

frame : frame_head_1 wss variables %dprec 3
      | frame_head_2 wss variables %dprec 4
      | frame_head_3 wss variables %dprec 5
      | frame_head_4 wss variables %dprec 2
      | frame_head_5 wss variables %dprec 1
;

frame_head : frame_head_1 %dprec 3
	   | frame_head_2 %dprec 4
	   | frame_head_3 %dprec 5
           | frame_head_4 %dprec 2
           | frame_head_5 %dprec 1
;

frame_head_1 : frame_start wss function_call wsa keyword_at wss file_location
             {
               $$ = frame_new();
               $$->number = $1;
               $$->function = $3->buf;
               strbuf_free_nobuf($3);
	       $$->sourcefile = $7->buf;
               strbuf_free_nobuf($7);
             }
;

frame_head_2 : frame_start wss frame_address_in_function wss keyword_at wss file_location
             {
               $$ = frame_new();
               $$->number = $1;
               $$->function = $3->buf;
               strbuf_free_nobuf($3);
	       $$->sourcefile = $7->buf;
               strbuf_free_nobuf($7);
             }
;

frame_head_3 : frame_start wss frame_address_in_function wss keyword_from wss file_location
             {
               $$ = frame_new();
               $$->number = $1;
               $$->function = $3->buf;
               strbuf_free_nobuf($3);
	       $$->sourcefile = $7->buf;
               strbuf_free_nobuf($7);
             }
;

frame_head_4 : frame_start wss frame_address_in_function
             {
               $$ = frame_new();
               $$->number = $1;
               $$->function = $3->buf;
               strbuf_free_nobuf($3);
             }
;

frame_head_5 : frame_start wss keyword_sighandler
             {
               $$ = frame_new();
               $$->number = $1;
	       $$->signal_handler_called = true;
             }

frame_start: '#' digit_sequence
                 {
                   if (sscanf($2->buf, "%d", &$$) != 1)
		   {
		     printf("Error while parsing frame number '%s'.\n", $2->buf);
		     exit(5);
		   }
		   strbuf_free($2);
                 }
;

frame_address_in_function : hexadecimal_number wss keyword_in wss function_call
                              {
                                strbuf_free($1);
                                $$ = $5;
                              }
                          | hexadecimal_number wss keyword_in wss keyword_vtable wss keyword_for wss function_call
                              {
                                strbuf_free($1);
                                $$ = $9;
                              }
;

file_location : file_name ':' digit_sequence
                  {
                    $$ = $1;
                    strbuf_free($3); /* line number not needed for now */
                  }
              | file_name
;

variables : variables_line '\n'
          | variables_line END
          | variables_line wss_nonl '\n'
          | variables_line wss_nonl END
          | variables variables_line '\n'
          | variables variables_line END
          | variables variables_line wss_nonl '\n'
          | variables variables_line wss_nonl END
          | variables wss_nonl variables_line '\n'
          | variables wss_nonl variables_line END
          | variables wss_nonl variables_line wss_nonl '\n'
          | variables wss_nonl variables_line wss_nonl END
;

variables_line : variables_char_no_framestart
               | variables_line variables_char
               | variables_line wss_nonl variables_char
;

variables_char : '#' | variables_char_no_framestart
;

/* Manually synchronized with function_args_char_base, except the first line. */
variables_char_no_framestart : digit | nondigit | '"' | '(' | ')' | '\\'
                             | '+' | '-' | '<' | '>' | '/' | '.'
                             | '[' | ']' | '?' | '\'' | '`' | ','
                             | '=' | '{' | '}' | '^' | '&' | '$'
                             | ':' | ';' | '!' | '@' | '*'
                             | '%' | '|' | '~'
;

function_call : function_name wss function_args %dprec 3
              | return_type wss_nonl function_name wss function_args %dprec 2
                  { $$ = $3; }
              | function_name wss_nonl identifier_template wss function_args %dprec 1
	      { $$ = $1; strbuf_free($3); }
;

return_type : identifier { strbuf_free($1); }
;

function_name : identifier
              | '?' '?'
                {
                  $$ = strbuf_new();
                  strbuf_append_str($$, "??");
                }
;

function_args : '(' wsa ')'
              | '(' wsa function_args_sequence wsa ')'
;

function_args_sequence : function_args_char
                       | function_args_sequence wsa '(' wsa ')'
                       | function_args_sequence wsa '(' wsa function_args_string wsa ')'
                       | function_args_sequence wsa '(' wsa function_args_sequence wsa ')'
                       | function_args_sequence wsa function_args_char
                       | function_args_sequence wsa function_args_string
;

function_args_string : '"' wsa function_args_string_sequence wsa '"'
                     | '"' wsa '"'
;

/* Manually synchronized with variables_char_no_framestart,
 * except the first line.
 */
function_args_char_base : digit | nondigit | '#'
                   | '+' | '-' | '<' | '>' | '/' | '.'
                   | '[' | ']' | '?' | '\'' | '`' | ','
                   | '=' | '{' | '}' | '^' | '&' | '$'
                   | ':' | ';' | '!' | '@' | '*'
                   | '%' | '|' | '~'
;
function_args_escaped_char : '\\' function_args_char_base
                           | '\\' '\\'
                           | '\\' '"'
;
function_args_char : function_args_char_base
                   | function_args_escaped_char
;


function_args_string_sequence : function_args_string_char
                    | function_args_string_sequence function_args_string_char
                    | function_args_string_sequence wss_nonl function_args_string_char
;

function_args_string_char : function_args_char | '(' | ')'
;

file_name : file_name_char { $$ = strbuf_new(); strbuf_append_char($$, $1); }
          | file_name file_name_char { $$ = strbuf_append_char($1, $2); }
;

file_name_char : digit | nondigit | '-' | '+' | '/' | '.'
;

 /* Function name, sometimes mangled.
  * Example: something@GLIB_2_2
  * CClass::operator=
  */
identifier : identifier_first_char %dprec 1
              {
		$$ = strbuf_new();
		strbuf_append_char($$, $1);
	      }
           | identifier_braces %dprec 1 /* e.g. (anonymous namespace)::WorkerThread */
           | identifier identifier_char %dprec 1
	      { $$ = strbuf_append_char($1, $2); }
           | identifier identifier_braces %dprec 1
              {
		$$ = strbuf_append_str($1, $2->buf);
		strbuf_free($2);
	      }
           | identifier identifier_template %dprec 2
              {
		$$ = strbuf_append_str($1, $2->buf);
		strbuf_free($2);
	      }
;

identifier_first_char: nondigit
                     | '~' /* destructor */
                     | '*'
;

identifier_char_no_templates : digit | nondigit | '@' | '.' | ':' | '='
	                     | '!' | '*' | '+' | '-' | '[' | ']'
                             | '~' | '&' | '/' | '%' | '^'
                             | '|' | ','
;

/* Most of the special characters are required to support C++
 * operator overloading.
 */
identifier_char : identifier_char_no_templates | '<'| '>'
;

identifier_braces : '(' ')'
                      {
			$$ = strbuf_new();
			strbuf_append_char($$, $1);
			strbuf_append_char($$, $2);
		      }
                  | '(' identifier_braces_inside ')'
                      {
			$$ = strbuf_new();
			strbuf_append_char($$, $1);
			strbuf_append_str($$, $2->buf);
			strbuf_free($2);
			strbuf_append_char($$, $3);
                      }
;

identifier_braces_inside : identifier_braces_inside_char %dprec 1
                            {
	                      $$ = strbuf_new();
		              strbuf_append_char($$, $1);
	                    }
                         | identifier_braces_inside identifier_braces_inside_char %dprec 1
			    { $$ = strbuf_append_char($1, $2); }
                         | identifier_braces_inside '(' identifier_braces_inside ')' %dprec 1
			    {
			      $$ = strbuf_append_char($1, $2);
			      $$ = strbuf_append_str($1, $3->buf);
			      strbuf_free($3);
			      $$ = strbuf_append_char($1, $4);
			    }
                         | identifier_braces_inside '(' ')' %dprec 1
			    {
			      $$ = strbuf_append_char($1, $2);
			      $$ = strbuf_append_char($1, $3);
			    }
                         | identifier_braces_inside identifier_template %dprec 2
			    {
			      $$ = strbuf_append_str($1, $2->buf);
			      strbuf_free($2);
			    }
;

identifier_braces_inside_char : identifier_char | ws_nonl
;

identifier_template : '<' identifier_template_inside '>'
                      {
			$$ = strbuf_new();
			strbuf_append_char($$, $1);
			strbuf_append_str($$, $2->buf);
			strbuf_free($2);
			strbuf_append_char($$, $3);
		      }
;

identifier_template_inside : identifier_template_inside_char
                              {
	                        $$ = strbuf_new();
		                strbuf_append_char($$, $1);
	                      }
                           | identifier_template_inside identifier_template_inside_char
			      { $$ = strbuf_append_char($1, $2); }
                           | identifier_template_inside '<' identifier_template_inside '>'
			      {
				$$ = strbuf_append_char($1, $2);
				$$ = strbuf_append_str($1, $3->buf);
				strbuf_free($3);
				$$ = strbuf_append_char($1, $4);
			      }
                           | identifier_template_inside identifier_braces
			      {
				$$ = strbuf_append_str($1, $2->buf);
				strbuf_free($2);
			      }
;

identifier_template_inside_char : identifier_char_no_templates | ws_nonl
;

digit_sequence : digit { $$ = strbuf_new(); strbuf_append_char($$, $1); }
               | digit_sequence digit { $$ = strbuf_append_char($1, $2); }
;

hexadecimal_number : '0' 'x' hexadecimal_digit_sequence
                     {
                       $$ = $3;
                       strbuf_prepend_str($$, "0x");
                     }
                   | '0' 'X' hexadecimal_digit_sequence
                     {
                       $$ = $3;
                       strbuf_prepend_str($$, "0X");
                     }
;

hexadecimal_digit_sequence : hexadecimal_digit
                               {
                                 $$ = strbuf_new();
                                 strbuf_append_char($$, $1);
                               }
                           | hexadecimal_digit_sequence hexadecimal_digit
                               { $$ = strbuf_append_char($1, $2); }
;

hexadecimal_digit : digit
                  | 'a' | 'b' | 'c' | 'd' | 'e' | 'f'
                  | 'A' | 'B' | 'C' | 'D' | 'E' | 'F'
;

digit : '0' | '1' | '2' | '3' | '4' | '5' | '6' | '7' | '8' | '9'
;

nondigit : 'a' | 'b' | 'c' | 'd' | 'e' | 'f' | 'g' | 'h' | 'i' | 'j' | 'k'
         | 'l' | 'm' | 'n' | 'o' | 'p' | 'q' | 'r' | 's' | 't' | 'u' | 'v' | 'w'
         | 'x' | 'y' | 'z'
         | 'A' | 'B' | 'C' | 'D' | 'E' | 'F' | 'G' | 'H' | 'I' | 'J' | 'K'
         | 'L' | 'M' | 'N' | 'O' | 'P' | 'Q' | 'R' | 'S' | 'T' | 'U' | 'V' | 'W'
         | 'X' | 'Y' | 'Z'
         | '_'
;

 /* whitespace */
ws : ws_nonl | '\n' | '\r'
;

 /* No newline.*/
ws_nonl : '\t' | ' '
;

 /* whitespace sequence without a newline */
wss_nonl : ws_nonl
         | wss_nonl ws_nonl
;

 /* whitespace sequence */
wss : ws
    | wss ws
;

 /* whitespace sequence allowed */
wsa :
    | wss
;

keyword_in : 'i' 'n'
;

keyword_at : 'a' 't'
;

keyword_for : 'f' 'o' 'r'
;

keyword_vtable : 'v' 't' 'a' 'b' 'l' 'e'
;

keyword_from : 'f' 'r' 'o' 'm'
;

keyword_thread: 'T' 'h' 'r' 'e' 'a' 'd'
;

keyword_sighandler: '<' 's' 'i' 'g' 'n' 'a' 'l' ' ' 'h' 'a' 'n' 'd' 'l' 'e' 'r' ' ' 'c' 'a' 'l' 'l' 'e' 'd' '>'
;

%%

static bool scanner_echo = false;
static char *yyin;

int yylex()
{
  char c = *yyin;
  if (c == '\0')
    return END;
  ++yyin;

  /* Debug output. */
  if (scanner_echo)
    putchar(c);

  yylval.c = c;

  /* Return a single char. */
  return c;
}

/* This is the function that is actually called from outside.
 * @returns
 *   Backtrace structure. Caller is responsible for calling
 *   backtrace_free() on this.
 *   Returns NULL when parsing failed.
 */
struct backtrace *backtrace_parse(char *input, bool debug_parser, bool debug_scanner)
{
  /* Skip the backtrace header information. */
  char *btnoheader_a = strstr(input, "\nThread ");
  char *btnoheader_b = strstr(input, "\n#");
  char *btnoheader = input;
  if (btnoheader_a)
  {
    if (btnoheader_b && btnoheader_b < btnoheader_a)
      btnoheader = btnoheader_b + 1;
    else
      btnoheader = btnoheader_a + 1;
  }
  else if (btnoheader_b)
    btnoheader = btnoheader_b + 1;

  /* Bug fixing hack for broken backtraces.
   * Sometimes the empty line is missing before new Thread section.
   * This is against rules, but a bug (now fixed) in Linux kernel caused
   * this.
   */
  char *thread_fixer = btnoheader + 1;
  while ((thread_fixer = strstr(thread_fixer, "\nThread")) != NULL)
  {
    if (thread_fixer[-1] != '\n')
      thread_fixer[-1] = '\n';

    ++thread_fixer;
  }

  /* Bug fixing hack for GDB - remove wrongly placed newlines from the backtrace.
   * Sometimes there is a newline in the local variable section.
   * This is caused by some GDB hooks.
   * Example: rhbz#538440
   * #1  0x0000000000420939 in sync_deletions (mse=0x0, mfld=0x1b85020)
   *    at mail-stub-exchange.c:1119
   *     status = <value optimized out>
   *     iter = 0x1af38d0
   *     known_messages = 0x1b5c460Traceback (most recent call last):
   *  File "/usr/share/glib-2.0/gdb/glib.py", line 98, in next
   *    if long (node["key_hash"]) >= 2:
   * RuntimeError: Cannot access memory at address 0x11
   *
   *     __PRETTY_FUNCTION__ = "sync_deletions"
   * #2  0x0000000000423e6b in refresh_folder (stub=0x1b77f10 [MailStubExchange],
   * ...
   *
   * The code removes every empty line (also those containing only spaces),
   * which is not followed by a new Thread section.
   *
   * rhbz#555251 contains empty lines with spaces
   */
  char *empty_line = btnoheader;
  char *c = btnoheader;
  while (*c)
  {
    if (*c == '\n')
    {
      char *cend = c + 1;
      while (*cend == ' ' || *cend == '\t')
        ++cend;
      if (*cend == '\n' && 0 != strncmp(cend, "\nThread", strlen("\nThread")))
        memmove(c, cend, strlen(cend) + 1);
    }
    ++c;
  }
  while ((empty_line = strstr(empty_line, "\n\n")) != NULL)
  {
    if (0 != strncmp(empty_line, "\n\nThread", strlen("\n\nThread")))
    {
      /* Remove the empty line by converting the first newline to char. */
      empty_line[0] = 'X';
    }
    ++empty_line;
  }

  /* Prepare for running parser. */
  g_backtrace = 0;
  yyin = btnoheader;
#if YYDEBUG == 1
  if (debug_parser)
    yydebug = 1;
#endif
  scanner_echo = debug_scanner;

  /* Parse. */
  int failure = yyparse();

  /* Separate debugging output. */
  if (scanner_echo)
    putchar('\n');

  if (failure)
  {
    if (g_backtrace)
    {
      backtrace_free(g_backtrace);
      g_backtrace = NULL;
    }
    fprintf(stderr, "Error while parsing backtrace.\n");
  }

  return g_backtrace;
}
