#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "http/http_util.h"
int main(void) {
 http_form_field_t f[4];
 assert(http_form_parse("a=1&b=two+words",f,4)==2);
 assert(!strcmp(f[1].val,"two words"));
 assert(http_form_parse("broken&b=value",f,4)==0);
 assert(http_form_parse("a=1&broken&b=value",f,4)==0);
 assert(http_form_parse("=value",f,4)==0);
 assert(http_form_parse("a=",f,4)==1 && !f[0].val[0]);
 assert(http_form_parse("a=%26%3D",f,4)==1 && !strcmp(f[0].val,"&="));
 char day[16],name[8];
 assert(http_split_relative("day/ok.log",day,sizeof(day),name,sizeof(name))==0);
 assert(http_split_relative("day/12345678",day,sizeof(day),name,sizeof(name))<0);
 assert(http_split_relative("../ok.log",day,sizeof(day),name,sizeof(name))<0);
 assert(http_split_relative("day/sub/file",day,sizeof(day),name,sizeof(name))<0);
}
