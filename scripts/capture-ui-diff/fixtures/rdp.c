/* Loopback RDP framebuffer for macOS capture, using the existing FreeRDP SDK.
 * Not a desktop/session server: only synthetic pixels and input are exposed. */
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/settings.h>
#include <freerdp/input.h>
#include <freerdp/update.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <winpr/ssl.h>
#include <winpr/synch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 1024
#define H 640
static const char *certificate, *keyfile, *pixels, *inputfile;
typedef struct { rdpContext context; BOOL active; } Context;

static BOOL activate(freerdp_peer *peer)
{
    ((Context *)peer->context)->active=TRUE;
    return TRUE;
}
static BOOL post_connect(freerdp_peer *peer)
{
    return freerdp_settings_set_uint32(peer->context->settings, FreeRDP_DesktopWidth, W)
        && freerdp_settings_set_uint32(peer->context->settings, FreeRDP_DesktopHeight, H);
}
static BOOL input_key(rdpInput *input, UINT16 flags, UINT8 code)
{
    (void)input;
    FILE *f=fopen(inputfile,"a");
    if(f){fprintf(f,"K %u %u\n",flags,code);fclose(f);}
    return TRUE;
}
static BOOL input_unicode(rdpInput *input, UINT16 flags, UINT16 code)
{
    (void)input;
    FILE *f=fopen(inputfile,"a");
    if(f){fprintf(f,"U %u %u\n",flags,code);fclose(f);}
    return TRUE;
}
static BOOL input_mouse(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
    (void)input;(void)flags;(void)x;(void)y;return TRUE;
}
static BOOL input_sync(rdpInput *input, UINT32 flags)
{
    (void)input;(void)flags;return TRUE;
}
static BOOL send_frame(freerdp_peer *peer)
{
    BYTE *frame=malloc(W*H*4);
    FILE *f=fopen(pixels,"rb");
    if(!frame||!f){free(frame);if(f)fclose(f);return TRUE;}
    size_t n=fread(frame,1,W*H*4,f);fclose(f);
    if(n!=W*H*4){free(frame);return TRUE;}
    BOOL ok=TRUE;
    /* Small raw tiles avoid protocol bitmap length limits and codec dependencies. */
    for(UINT32 y=0;y<H&&ok;y+=8){
        BITMAP_DATA data={0};
        data.destLeft=0;data.destRight=W-1;data.destTop=y;data.destBottom=y+7;
        data.width=W;data.height=8;data.bitsPerPixel=32;data.bitmapLength=W*8*4;
        BYTE tile[W*8*4];
        for(UINT32 row=0;row<8;row++)memcpy(tile+row*W*4,frame+(y+7-row)*W*4,W*4);
        data.bitmapDataStream=tile;data.compressed=FALSE;
        BITMAP_UPDATE update={0};update.number=1;update.rectangles=&data;update.skipCompression=TRUE;
        ok=peer->context->update->BitmapUpdate(peer->context,&update);
    }
    free(frame);return ok;
}
static void *serve(void *arg)
{
    freerdp_peer *peer=arg;
    peer->ContextSize=sizeof(Context);
    if(!freerdp_peer_context_new(peer))goto done;
    rdpSettings *s=peer->context->settings;
    rdpPrivateKey *key=freerdp_key_new_from_file_enc(keyfile,NULL);
    rdpCertificate *cert=freerdp_certificate_new_from_file(certificate);
    if(!key||!cert)goto context_done;
    if(!freerdp_settings_set_pointer_len(s,FreeRDP_RdpServerRsaKey,key,1)
       ||!freerdp_settings_set_pointer_len(s,FreeRDP_RdpServerCertificate,cert,1)
       ||!freerdp_settings_set_bool(s,FreeRDP_RdpSecurity,FALSE)
       ||!freerdp_settings_set_bool(s,FreeRDP_TlsSecurity,TRUE)
       ||!freerdp_settings_set_bool(s,FreeRDP_NlaSecurity,FALSE)
       ||!freerdp_settings_set_bool(s,FreeRDP_ExtSecurity,FALSE)
       ||!freerdp_settings_set_uint32(s,FreeRDP_ColorDepth,32)
       ||!freerdp_settings_set_uint32(s,FreeRDP_DesktopWidth,W)
       ||!freerdp_settings_set_uint32(s,FreeRDP_DesktopHeight,H))goto context_done;
    peer->PostConnect=post_connect;peer->Activate=activate;
    peer->context->input->SynchronizeEvent=input_sync;
    peer->context->input->KeyboardEvent=input_key;
    peer->context->input->UnicodeKeyboardEvent=input_unicode;
    peer->context->input->MouseEvent=input_mouse;
    if(!peer->Initialize(peer))goto context_done;
    for(;;){
        HANDLE handles[32];DWORD count=peer->GetEventHandles(peer,handles,32);
        if(!count||WaitForMultipleObjects(count,handles,FALSE,200)==WAIT_FAILED)break;
        if(!peer->CheckFileDescriptor(peer))break;
        if(((Context *)peer->context)->active&&!send_frame(peer))break;
    }
    peer->Disconnect(peer);
context_done:
    freerdp_peer_context_free(peer);
done:
    freerdp_peer_free(peer);return NULL;
}
static BOOL accepted(freerdp_listener *listener,freerdp_peer *peer)
{
    (void)listener;pthread_t thread;
    if(pthread_create(&thread,NULL,serve,peer)!=0)return FALSE;
    pthread_detach(thread);return TRUE;
}
int main(int argc,char **argv)
{
    if(argc!=6)return 2;
    certificate=argv[2];keyfile=argv[3];pixels=argv[4];inputfile=argv[5];
    if(!winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT))return 3;
    freerdp_listener *listener=freerdp_listener_new();
    if(!listener)return 4;
    listener->PeerAccepted=accepted;
    if(!listener->Open(listener,"127.0.0.1",(UINT16)atoi(argv[1])))return 5;
    for(;;){
        HANDLE handles[32];DWORD count=listener->GetEventHandles(listener,handles,32);
        if(!count||WaitForMultipleObjects(count,handles,FALSE,1000)==WAIT_FAILED)break;
        if(!listener->CheckFileDescriptor(listener))break;
    }
    listener->Close(listener);freerdp_listener_free(listener);return 0;
}
