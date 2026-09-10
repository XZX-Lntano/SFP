#include "bridge_protocol.h"
size_t fixture_frame(uint8_t *out,const uint8_t *app,unsigned worker) {
    const uint8_t dst[6]={255,255,255,255,255,255};
    return fpga_frame(out,app,worker,dst,0xc0a80a01+worker,0xc0a80a65+worker);
}
int fixture_parse(const uint8_t *frame,size_t len,uint8_t *out) {
    uint32_t base;unsigned rounds,workers;
    return fpga_result(frame,len,out,&base,&rounds,&workers);
}
int fixture_app(const uint8_t *app,size_t n,int response) {return app_valid(app,n,response);}
