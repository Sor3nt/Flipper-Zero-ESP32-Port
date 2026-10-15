/* Optional private-response replay. Never bundles or prints response contents. */
#include "wdgwars_map_json.h"
#include "wdgwars_map_render.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(int argc,char** argv) {
    assert(argc==4);
    FILE* file=fopen(argv[1],"rb"); assert(file);
    char* body=malloc(WDG_MAP_RESPONSE_MAX+1); assert(body);
    size_t length=fread(body,1,WDG_MAP_RESPONSE_MAX+1,file); assert(!ferror(file)); fclose(file);
    assert(length && length<=WDG_MAP_RESPONSE_MAX);
    WdgMapSnapshot* map=calloc(1,sizeof(*map)); assert(map);
    char error[96]; bool overview=!strcmp(argv[3],"overview");
    bool ok=overview?wdg_map_parse_overview(body,length,map,error,sizeof(error)):
        wdg_map_parse_territories(body,length,map,error,sizeof(error));
    free(body);
    if(!ok) { fprintf(stderr,"Response rejected: %s\n",error); free(map); return 1; }
    if(overview) map->bounds=(WdgMapBounds){-85.051128,-180,85.051128,180};
    WdgMapRenderState view={.zoom=overview?0:14,.online=false,.cache_age_known=true,.area_name="Saved territories"};
    view.center_lat=(map->bounds.south+map->bounds.north)/2;
    view.center_lon=(map->bounds.west+map->bounds.east)/2;
    map->zoom=(uint8_t)view.zoom;
    size_t n=WDG_MAP_DISPLAY_WIDTH*WDG_MAP_DISPLAY_HEIGHT;
    uint16_t* pixels=calloc(n+32,sizeof(*pixels)); assert(pixels);
    for(size_t i=0;i<16;++i) pixels[i]=pixels[n+16+i]=0xa55a;
    clock_t started=clock(); wdgwars_map_render(pixels+16,map,&view,NULL);
    printf("Response replay: %u cells, %u gangs, %u hulls, %u vertices; native render %.1f ms\n",
        map->cell_count,map->gang_count,map->hull_count,map->point_count,
        1000.0*(clock()-started)/CLOCKS_PER_SEC);
    for(size_t i=0;i<16;++i) assert(pixels[i]==0xa55a && pixels[n+16+i]==0xa55a);
    file=fopen(argv[2],"wb");assert(file);fprintf(file,"P6\n320 170\n255\n");
    for(size_t i=0;i<n;++i) { uint16_t c=pixels[i+16];c=(uint16_t)((c>>8)|(c<<8));
        unsigned char rgb[3]={(unsigned char)(((c>>11)&31)*255/31),(unsigned char)(((c>>5)&63)*255/63),(unsigned char)((c&31)*255/31)};
        assert(fwrite(rgb,1,3,file)==3);
    }
    assert(!fclose(file));free(pixels);free(map);return 0;
}
