/* d9mt v2 shader assembler: assembles D3D9 shader assembly text (vs_3_0 /
 * ps_3_0) into bytecode via D3DXAssembleShader and writes it to a file. Runs
 * under Wine in the bottle (which has d3dx9). Lets the test suite author shaders
 * with controlled outputs (explicit alpha, multiple render targets, cube
 * samplers) that no precompiled .vso/.pso provides.
 *
 *   asm_shader.exe <input.asm> <output.bin>
 */
#define COBJMACROS
#include <windows.h>
#include <d3dx9.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: asm_shader <input.asm> <output.bin>\n");
    return 2;
  }

  FILE *in = fopen(argv[1], "rb");
  if (!in) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  fseek(in, 0, SEEK_END);
  long len = ftell(in);
  fseek(in, 0, SEEK_SET);
  char *src = (char *)malloc(len + 1);
  fread(src, 1, len, in);
  src[len] = 0;
  fclose(in);

  ID3DXBuffer *shader = NULL, *errors = NULL;
  HRESULT hr = D3DXAssembleShader(src, (UINT)len, NULL, NULL, 0, &shader, &errors);
  if (FAILED(hr) || !shader) {
    if (errors)
      fprintf(stderr, "assemble failed: %s\n", (const char *)errors->lpVtbl->GetBufferPointer(errors));
    else
      fprintf(stderr, "assemble failed: 0x%08lx\n", (unsigned long)hr);
    return 1;
  }

  FILE *out = fopen(argv[2], "wb");
  if (!out) {
    fprintf(stderr, "cannot write %s\n", argv[2]);
    return 2;
  }
  fwrite(shader->lpVtbl->GetBufferPointer(shader), 1,
         shader->lpVtbl->GetBufferSize(shader), out);
  fclose(out);
  printf("assembled %lu bytes -> %s\n",
         (unsigned long)shader->lpVtbl->GetBufferSize(shader), argv[2]);
  return 0;
}
