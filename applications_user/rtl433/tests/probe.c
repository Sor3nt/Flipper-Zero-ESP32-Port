#include "../rtl_engine.h"
#include <stdio.h>
static void output(void *ctx,const r_device *d,const data_t *v,const RtlMeta *m) {(void)ctx;(void)d;(void)v;(void)m;}
int main(void) {RtlEngine e; if(!rtl_engine_init(&e,output,0,0))return 1;unsigned enabled=0;for(size_t i=0;i<e.count;i++)enabled+=e.enabled[i];printf("%zu catalog entries, %u default-enabled\n",e.count,enabled);rtl_engine_free(&e);return 0;}
