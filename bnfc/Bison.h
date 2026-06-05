/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_GRAMMAR_BISON_H_INCLUDED
# define YY_GRAMMAR_BISON_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 1
#endif
#if YYDEBUG
extern int grammar_debug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    _ERROR_ = 258,                 /* _ERROR_  */
    _BANG = 259,                   /* _BANG  */
    _AMP = 260,                    /* _AMP  */
    _DAMP = 261,                   /* _DAMP  */
    _LPAREN = 262,                 /* _LPAREN  */
    _RPAREN = 263,                 /* _RPAREN  */
    _SEMI = 264,                   /* _SEMI  */
    _LT = 265,                     /* _LT  */
    _GT = 266,                     /* _GT  */
    _DGT = 267,                    /* _DGT  */
    _KW_elif = 268,                /* _KW_elif  */
    _KW_else = 269,                /* _KW_else  */
    _KW_fi = 270,                  /* _KW_fi  */
    _KW_if = 271,                  /* _KW_if  */
    _KW_then = 272,                /* _KW_then  */
    _KW_time = 273,                /* _KW_time  */
    _LBRACE = 274,                 /* _LBRACE  */
    _BAR = 275,                    /* _BAR  */
    _DBAR = 276,                   /* _DBAR  */
    _RBRACE = 277,                 /* _RBRACE  */
    T_Assign = 278,                /* T_Assign  */
    T_Word = 279                   /* T_Word  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 77 "Grammar.y"

  int    _int;
  char   _char;
  double _double;
  char*  _string;
  Input input_;
  Job job_;
  ListJob listjob_;
  OptElse optelse_;
  Condition condition_;
  NegCmd negcmd_;
  CommandLine commandline_;
  OptRedir optredir_;
  Pipeline pipeline_;
  CommandPart commandpart_;
  ListWord listword_;

#line 106 "Bison.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif

/* Location type.  */
#if ! defined YYLTYPE && ! defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE YYLTYPE;
struct YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
};
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif




int grammar_parse (yyscan_t scanner, YYSTYPE *result);


#endif /* !YY_GRAMMAR_BISON_H_INCLUDED  */
