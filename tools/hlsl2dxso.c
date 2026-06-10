/* Compiles HLSL to D3D9 SM1-3 bytecode via d3dcompiler. Runs under wine.
 * usage: hlsl2dxso.exe <in.hlsl> <profile e.g. vs_3_0|ps_3_0> <out.bin>
 */
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: hlsl2dxso <in.hlsl> <profile> <out.bin>\n");
    return 2;
  }

  FILE *f = fopen(argv[1], "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *src = malloc(len);
  fread(src, 1, len, f);
  fclose(f);

  ID3DBlob *code = NULL, *errors = NULL;
  HRESULT hr = D3DCompile(src, len, argv[1], NULL, NULL, "main", argv[2],
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (errors)
    fprintf(stderr, "%s", (char *)ID3D10Blob_GetBufferPointer(errors));
  if (FAILED(hr) || !code) {
    fprintf(stderr, "D3DCompile failed: 0x%08lx\n", (unsigned long)hr);
    return 1;
  }

  FILE *out = fopen(argv[3], "wb");
  fwrite(ID3D10Blob_GetBufferPointer(code), 1,
         ID3D10Blob_GetBufferSize(code), out);
  fclose(out);
  printf("%s: %lu bytes\n", argv[3],
         (unsigned long)ID3D10Blob_GetBufferSize(code));
  return 0;
}
