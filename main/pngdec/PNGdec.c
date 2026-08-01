//
// PNG Decoder - ESP-IDF C wrapper
//
#include "PNGdec.h"

// forward references
PNG_STATIC int PNGInit(PNGIMAGE *pPNG);
PNG_STATIC int DecodePNG(PNGIMAGE *pImage, void *pUser, int iOptions);
PNG_STATIC uint8_t PNGMakeMask(PNGDRAW *pDraw, uint8_t *pMask, uint8_t ucThreshold);
// Include the C code which does the actual work
#include "png.inl"

// C API wrappers (png.inl uses DecodePNG internally, but header declares PNG_decode)
int PNG_decode(PNGIMAGE *pPNG, void *pUser, int iOptions) {
    return DecodePNG(pPNG, pUser, iOptions);
}
int PNG_getBpp(PNGIMAGE *pPNG) { return (int)pPNG->ucBpp; }
int PNG_getPixelType(PNGIMAGE *pPNG) { return (int)pPNG->ucPixelType; }
int PNG_hasAlpha(PNGIMAGE *pPNG) { return pPNG->iHasAlpha; }
int PNG_isInterlaced(PNGIMAGE *pPNG) { return pPNG->iInterlaced; }
void PNG_setBuffer(PNGIMAGE *pPNG, uint8_t *pBuffer) { pPNG->pImage = pBuffer; }
