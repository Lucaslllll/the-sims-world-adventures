# Dependências e créditos

## Código incluído no repositório

| Componente | Versão | Origem | Licença preservada |
|---|---|---|---|
| SDL2 | 2.30.9 | https://github.com/libsdl-org/SDL | `android/third_party/SDL2-2.30.9/LICENSE.txt` (zlib) |
| OpenAL Soft | 1.23.1 | https://github.com/kcat/openal-soft | `android/third_party/openal-soft-1.23.1/COPYING` e `BSD-3Clause` |
| stb_image | Versão vendorizada | https://github.com/nothings/stb | Termos MIT/domínio público no próprio `android/third_party/stb_image.h` |

Cabeçalhos, créditos e arquivos de licença dos componentes de terceiros são mantidos.
O wrapper do Gradle também acompanha o projeto para reproduzir a compilação.

## Referência do port

A base HLE/SDL foi reutilizada do projeto local de The Sims 3 Ambitions. O registro
dos arquivos de origem e seus hashes, antes das adaptações de pacote/ABI, está em
[`reference-source.json`](reference-source.json). A adaptação deste projeto inclui o
loader Mach-O ARM32, ponte de ABI Darwin32/Android, gráficos, controles e importação.

## Ferramentas e dependências externas

- **Unicorn 2.1.4:** executor de CPU da pesquisa ARM64, derivado do QEMU. O builder
  baixa a fonte fixada por hash e preserva seu `COPYING`; não está embutido no APK ARM32.
- **Capstone:** análise de instruções no experimento Python.
- **Pillow e NumPy:** processamento de imagens e documentação gráfica.
- **Real-ESRGAN ncnn Vulkan:** processamento offline do pacote HD, conforme
  [`06-GRAFICOS-HD.md`](06-GRAFICOS-HD.md). A ferramenta/modelos são obtidos separadamente.

Os requisitos Python ficam em `tools/requirements.txt` e `tools/requirements-graphics.txt`.

## Conteúdo do jogo

The Sims 3 World Adventures, suas marcas e seus recursos pertencem aos respectivos
titulares. Este projeto comunitário é independente e não inclui o jogo. Cada jogador
fornece sua própria cópia compatível; o APK é separado dos dados importados.
As capturas nos guias servem como evidência visual das funcionalidades testadas.
