# Sprints — World Adventures para Android

Registro iniciado em 2026-10-12. Evidências de execução prevalecem sobre hipóteses.

## Sprint 0 — Diagnóstico e referência (concluída)

- Referência analisada: projeto local `the-sims-ambitions`.
- Ambitions executa uma fatia ARM64 nativamente e reimplementa APIs iOS em C++.
- World Adventures fornecido: versão 1.0.0, mínimo iOS 2.2.1, SDK iOS 3.0.
- Executável `sims3wa_iphone`: Mach-O ARMv6 **32-bit**, sem fatia ARM64.
- FairPlay: `cryptid=0`; o conteúdo de código pode ser analisado diretamente.
- Loader legado: `LC_SEGMENT`, `LC_SYMTAB`, `LC_DYSYMTAB`, `LC_UNIXTHREAD`.
  Não usa o fluxo `LC_DYLD_INFO`/`LC_MAIN` do Ambitions.
- Dispositivo: Galaxy A10 SM-A105F, Android 10, ABI ARM64 e ARM32.
- Ferramenta inicial: `tools/analyze_ipa.py`, análise e extração da cópia local.
- Relatório gerado: 480 imports reais, 1.245 relocações externas, 43 classes e
  796 referências a seletores; 1.525 arquivos no bundle (127,4 MiB).
- Binário não PIE com base `0x1000`; Android bloqueia mapas abaixo de `0x8000`.
- `_main` identificado em `0xcb1f0`; entrada Unix não é uma função C.
- Base de código do Ambitions importada com registro de proveniência.

**Critério de conclusão:** relatório reproduzível com segmentos, imports, relocações,
metadados ObjC e diferenças de ABI documentadas.

## Sprint 1 — Loader ARM32 e primeiro boot (concluída)

- Reutilizar a camada HLE/SDL do Ambitions com pacote separado `com.ts3wa.port`.
- Carregar Mach-O ARM32 e resolver as tabelas de símbolos/relocações antigas.
- Adaptar layout ObjC2, mensagens ARM/Thumb, tipos CoreGraphics e varargs.
- Compilar `armeabi-v7a`, instalar no A10 e registrar onde o código original chega.

### Implementação e evidências

- APK instalado em `com.ts3wa.port`, execução nativa ARM32, sem root.
- 43 classes ObjC realizadas, 28/28 construtores executados, `_main` e
  `UIApplicationMain` atingidos. Menu renderizado por GLES 1.1 na Mali-G71.
- Loader resolve 1.657 locais de bind; sete símbolos continuam sem implementação.
  Classes de frameworks adicionais podem ser placeholders; não são 480 APIs completas.
- Fontes: logs `analysis/device/boot-20261012-095950.log` e posteriores.

Problemas resolvidos: ponteiros dos stubs ARM; bit Thumb em funções locais;
literais PIC que não devem ser rebased; `__objc_superrefs`; tabelas de switch ARM
com destinos absolutos; pilha Apple alinhada a 4 versus bionic a 8 bytes.
`loader/abi32.S` realinha as chamadas HLE e preserva até 128 bytes de argumentos.

**Critério de conclusão:** código original executando, inicialização identificada e
diagnóstico de falhas persistente. Um APK que só abre o importador não conta como boot.

## Sprint 2 — Menu e partida (marco básico concluído; cobertura ampliada na Sprint 3)

- Corrigir APIs realmente chamadas, textos, gráficos GLES1/PVRTC e toque.
- Validar menu, criação de Sim, mundo e tutorial.
- Salvar/carregar e entrada de nome via teclado Android.

### Validado em 2026-10-12

- Menu, escolha de slot e criação de Sim com modelo/animação/espelho 3D.
- Persona, seleção de cinco traços, teclado Android e nome de teste entregue ao jogo.
- Mundo da casa, caminhada por toque, menu e ação de sentar no sofá.
- Prompts do tutorial e ajuda inicial; não foi percorrido o tutorial completo.
- Cidade, rolagem da câmera, controle de zoom e cenário do aeroporto renderizado.
- Save explícito: `sim3dat0` e `sim3set` atualizados às 16:15; após reiniciar o processo,
  o mundo foi carregado com a configuração salva e sem repetir o prompt do tutorial.
- Retorno ao jogo após ir à Home e após o usuário abrir outro aplicativo.
- ~60 FPS em trechos estáveis de menu/CAS/casa/cidade a 480×320; carregamentos têm quedas.
- PSS medido: ~104 MiB no menu e ~107 MiB no mundo. Não é medição de sessão longa.

**Correções importantes:** o ID M3G `0x2741` não é o ponteiro ARM `0x2740` com bit Thumb;
e tabelas de shorts como `0x00140013` são dois números, não endereços ObjC. Alterá-las
causava crashes na criação do Sim e na carga do mundo. A regressão nativa em
`tools/tests/loader_regression.cpp` carrega o Mach-O real e verifica os casos, sem executar
código ARM no host. Saves construídos como `files/Documents/...` são redirecionados
para `files/home/Documents/...`. Português sem recursos nesta versão cai para inglês.

Evidências visuais em `docs/img/`, logs completos em `analysis/device/`.

**Critério de conclusão:** partida controlável com evidências visuais e log.

## Sprint 3 — Viagens, áudio e ciclo de vida (em andamento, validação pelo usuário)

- Validar os destinos, transições, missões, inventário, minijogos e saves.
- Música CAF/AAC, efeitos OpenAL, pausa/retomada e sessão prolongada.
- AudioQueue/AAC e OpenAL inicializam nos logs; confirmação auditiva ainda pendente.
- Viagem a outro país ainda não validada. O avião do aeroporto aparece e sai
  periodicamente; o prédio não abriu uma opção de viagem nos toques testados.
- Usuário está jogando e reportará erros. Captura passiva em
  `analysis/device/user-validation-20261012.log`; não reiniciar/atualizar o app durante isso.

## Sprint 4 — Android exclusivamente ARM64 (prova de CPU no PC concluída; ponte Android pendente)

O IPA atual não tem código ARM64. A versão ARM32 para o A10 é um marco de desenvolvimento,
não comprova compatibilidade com aparelhos que removeram o suporte a aplicativos 32-bit.
Avaliar execução traduzida/emulada de ARM32 com ponte para a HLE, ou uma versão legítima
mais recente do jogo se existir e tiver outra arquitetura. A decisão depende da análise.

`tools/emulate_boot.py` com Unicorn 2.1.4 executou os 28 construtores e o `_main` originais
num host x86-64 de 64 bits até `UIApplicationMain`, com delegate correto. Executou também
os métodos originais de UIDevice(Hardware). Imports desconhecidos falham explicitamente.
Resultado e limitações em [05-EXECUCAO-EMULADA.md](05-EXECUCAO-EMULADA.md).
Esse experimento não equivale a um APK ARM64 jogável nem a teste em CPU ARM64.

Core Unicorn e um probe C também foram compilados para Android `arm64-v8a`/API26.
ELF64/AArch64 e alinhamento de segmentos 16 KiB verificados com llvm-readelf. O mesmo
probe compilado em Linux64 executou um método original M3G e conferiu o retorno 20.
Executar o probe ARM64 no aparelho e ligar a HLE continuam pendentes.

**Critério de conclusão:** APK ARM64 rodando o jogo em aparelho sem runtime ARM32,
com desempenho e memória medidos.

## Sprint 5 — Distribuição comunitária (planejada)

- APK separado dos dados do jogo, importador validado e build reproduzível.
- Guia de jogador, matriz de compatibilidade e licença definida pelo autor.
- Nenhum IPA, asset ou executável original incluído no APK/repositório.

### Artefato experimental já preparado

- Release compilado com sucesso e `lintVitalRelease` executado.
- `dist/TS3WorldAdventures-0.0.1-arm32-experimental.apk`, 1.528.068 bytes (~1,46 MiB).
- APK validado por `apksigner` (assinatura v2, chave de debug), `aapt` e auditoria ZIP/ELF.
- Pacote `com.ts3wa.port`, mínimo Android 26, target 33, somente `armeabi-v7a`.
- Nenhum arquivo em `assets/`; executável e dados do jogo continuam importados à parte.
- SHA-256: `3764b6146c05a68d8fbbd1b71aebe7b68522d597413af54135dcae4ec787767c`.
- O usuário está validando o debug já instalado. O release otimizado ainda não foi
  instalado/testado em gameplay, para preservar a sessão de validação em andamento.

## Verificações consolidadas

- Nove testes Python no venv: análise, startup emulado, falha em import desconhecido
  e retorno de 64 bits zero — todos passaram.
- Regressão C++ do loader nativo — passou.
- Build debug — instalado e validado nos fluxos descritos.
- Build release + lintVital + assinatura/metadados/ELF/ZIP — passaram no computador.
- Build executor C Linux64 + execução do método original — passou.
- Build executor Android ARM64 + inspeção ELF/16 KiB — passou; execução no Android pendente.

## Sprint 6 — Gráficos HD (primeira versão concluída em 2026-10-13)

Solicitação: extrair e ampliar imagens/sprites/texturas para melhorar a qualidade.

- Extração de 1.014 imagens, 979 únicas, incluindo 270 Image2D M3G e 64 PNG CgBI.
- Upscale Real-ESRGAN 2× concluído para as 979 imagens, com transparência do original.
- Pacote local externo de ~272,3 MiB com 4.264 hashes de uploads originais.
- Renderização física 960×640 com interface lógica 480×320, color/depth e scissor compatíveis.
- Substituição no GL, UVs/metadados originais, mipmaps coerentes e subupdates escalados.
- Fallback filtrado 2× para texturas dinâmicas ausentes do catálogo AI.
- Debug e release 0.0.2 testados no A10: menu e cenário egípcio, ~60 FPS estáveis.
- O save do usuário já estava num destino egípcio; foi carregado para a comparação.
- Saves preservados byte a byte, backups em `analysis/graphics/`.
- 13 testes Python e regressão C++ gráfica passaram; comparações visuais arquivadas.

Detalhes e reprodução: [06-GRAFICOS-HD.md](06-GRAFICOS-HD.md).
Limites: 23 DAT proprietários não extraídos; fontes dinâmicas partem da base original;
artefatos de IA a validar em mais sprites; importação do HD ZIP pelo seletor Android
e perfis acima de 2× ainda pendentes.

## Sprint 7 — Animação, fontes HD e tela (concluída em 2026-10-13)

Solicitação: corrigir a T-pose, melhorar fontes/ícones e avaliar preenchimento de tela,
preservando a qualidade do cenário já aprovada pelo usuário.

### T-pose e constantes numéricas

- Reproduzida com HD desligado. Não era causada pelo upscale.
- O literal `0x8840` em `0xe06dc` é `GL_MATRIX_PALETTE_OES`; coincidia com o endereço
  ARM de `ByteArrayInputStream::reset` e era rebased indevidamente.
- `GlobalConstants::ANIMFACTOR_CRASHED` em `0x126cb4` contém `0xc000` (0,75 em 16.16).
  O rebase o confundia com `CompositingMode::getClass`, produzindo avanço negativo de animação.
- Preservação total de `__TEXT,__const`: constantes/strings não são tabelas de ponteiros.
  Os fixups de vtables em `__DATA,__const` continuam; ponteiro legítimo de reset em `0x138b10`
  continua rebased. O total reconstruído passou de 17.022 para **17.007 slots**.
- Regressão compara toda a seção original byte a byte e cobre os dois conflitos.
- Hooks opcionais `trace_animation=1` acompanham start/update/setTime. Capturas/log em
  `analysis/pose-tests/fixed-constants/` mostram braços normais e deltas positivos (~11–14 ms).
  Diagnóstico desligado no perfil normal.

### Fonte e ícones

- Glifos rasterizados nativamente em 2× pelo Canvas Android, medidas lógicas preservadas.
- Contextos bitmap HD, sincronização com buffer do jogo e cache LRU de até 32 MiB.
- Correção do upload de texto: alocação GL vazia seguida de subimage completo agora usa
  a cópia HD. Confirmado por `HD glyph texture` no menu, HUD e diálogos.
- Refinamento offline 4×→2× de 12 atlas de HUD/ícones, com alpha original.
  As outras 967 imagens são byte a byte idênticas ao pacote anterior.

### Tela e validação

- `screen_mode=fit` mantém proporções; `stretch` preenche a janela, distorcendo a proporção.
- **Voltar do Android → Tela** aplica e persiste a opção durante a sessão. Atalho Tela
  declarado para launchers compatíveis. Não altera projeção/UI para widescreen real.
- Fit/stretch e áreas de toque exercitados no A10; perfil fit restaurado.
- Debug e release 0.0.3 instalados/testados: menus e mundo da casa a ~60 FPS estáveis;
  decodificação/carregamento ainda têm quedas. PSS release no mundo ~100,8 MiB na amostra.
- Hashes de `sim3dat0` e `sim3set` permaneceram iguais ao backup anterior aos testes.
- 13 testes Python, regressão loader, regressão gráfica e regressão viewport passaram.
  Builds debug/release, `lintVitalRelease`, assinatura v2 e auditoria ZIP/ELF passaram.
- Capturas/logs em `analysis/ui-tests/`; evidências publicáveis em `docs/img/ui/`.

### Artefatos

- `dist/TS3WorldAdventures-0.0.3-arm32-hd-experimental.apk`, **1.550.804 bytes**, sem assets.
- SHA-256 APK: `77e3e4665279c11cbb091027c5bd03bfaf8a2b0e506c98a3af42e060482faa0d`.
- `dist/TS3WorldAdventures-HD2x-UI-0.0.3-local.zip`, ~272,1 MiB, separado do APK.
- SHA-256 pacote: `b3ca9fde244469b7a36bd6a418dd43787a3fcf2ea9d7735cce52e849c6ea824c`.
- Release usa chave debug, como os anteriores; atualiza via `adb install -r`.
  Artefatos 0.0.1/0.0.2 preservados. Viagens/minijogos/sessão longa e ARM64 seguem nas sprints próprias.

Reprodução e opções: [06-GRAFICOS-HD.md](06-GRAFICOS-HD.md).

## Ajuste visual — ícone World Adventures (0.0.4, 2026-10-14)

- Novo desenho autoral: plumbob verde facetado, globo azul, bússola dourada, avião
  e selo 3. Segue a família visual do ícone do port de Ambitions.
- Camadas vetoriais de ícone adaptativo, `roundIcon` e PNGs de 48/72/96/144/192 pixels.
  Regeneração por `tools/gen_icon.py`; prévias em `docs/img/icone-world-adventures*.png`.
- Release e `lintVitalRelease` passaram; assinatura v2 e metadados verificados.
- 0.0.4 instalado via `adb install -r` no A10, conferido na lista de aplicativos
  com a máscara octogonal do launcher. Logs/capturas locais em `analysis/icon-tests/`.
- Backup de `home` antes da atualização; hashes dos dois saves idênticos depois.
- `dist/TS3WorldAdventures-0.0.4-arm32-hd-experimental.apk`, 1.612.420 bytes.
  SHA-256: `c5f5f7b9c3d06c05654dd8dd151561573eb77186eb855c51d0eb8eb29c4dddcb`.
- Bibliotecas nativas têm os mesmos hashes da 0.0.3; validação desta revisão focada
  nos recursos do launcher. A partida não foi iniciada para essa conferência.
