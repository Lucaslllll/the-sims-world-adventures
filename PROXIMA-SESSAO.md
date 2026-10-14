# Continuidade — World Adventures

Notas técnicas de continuidade do projeto `the-sims-world-adventures`.
Ler `docs/SPRINTS.md`, `docs/02-ARQUITETURA.md` e `docs/03-COMPILAR.md`.

## Já funcionando

**Atualização 2026-10-14:** Sprints 6/7 concluídas. Release **0.0.4 ARM32** instalado no A10
com 979 texturas Real-ESRGAN 2× em `files/hdpack/`, 4.264 chaves de uploads, renderização
960×640 preservando layout lógico 480×320. Menu e mundo da casa a ~60 FPS estáveis.
Ver `docs/06-GRAFICOS-HD.md`, comparação em `docs/img/hd/jogo-comparacao.png`.
Saves comparados byte a byte com backup: preservados. Pacote local em `dist/` (~272 MiB).
Fontes dinâmicas elegíveis agora são rasterizadas nativamente em 2×, com cache LRU de 32 MiB;
`HD glyph texture` confirma publicação pelo subimage completo de texturas inicialmente vazias.
12 atlas de UI refinados por IA 4×→2×; outras 967 imagens idênticas ao pacote original HD.
Pacote novo em `analysis/graphics/hdpack-ui/` e `dist/TS3WorldAdventures-HD2x-UI-0.0.3-local.zip`.
23 DAT da loja/outros jogos seguem não extraídos.
Configuração: `render_scale`, `texture_upscale`, `text_scale` aceitam 1–4, padrão testado 2.
`screen_mode=fit` (restaurado no telefone) ou `stretch`, aplicado por **Voltar Android → Tela**.
Modo stretch preenche, mas distorce proporções; não é widescreen real. Atalho Tela no ícone do app.
13 testes Python e três regressões C++ (loader, GL, viewport) passaram.

0.0.4 atualiza o ícone: plumbob, globo/bússola e avião. Vetores adaptativos e PNGs de
todas as densidades, gerados por `tools/gen_icon.py`. Prévia em `docs/img/icone-world-adventures.png`.
Conferido no launcher do A10 em 2026-10-14; bibliotecas nativas idênticas à 0.0.3.
APK novo em `dist/TS3WorldAdventures-0.0.4-arm32-hd-experimental.apk`; 1.612.420 bytes,
SHA-256 `c5f5f7b9c3d06c05654dd8dd151561573eb77186eb855c51d0eb8eb29c4dddcb`.

Ferramentas: `extract_graphics.py`, `setup_upscaler.py`, `upscale_graphics.py`, `refine_ui.py`, `hdpack.py`, `ui_report.py`.
Requisitos gráficos em `tools/requirements-graphics.txt`. HD ZIP pelo seletor Android
continua pendente; deploy pelo script/ADB. Não embutir texturas do jogo no APK/repositório.

Port nativo **ARM32** do IPA iPhone World Adventures 1.0.0 no Galaxy A10/Android 10.
Menu, criar Sim, teclado Android/entrada de nome, casa, caminhar/sentar, cidade, câmera/zoom,
salvar/carregar (confirmado timestamp e reinício), retomada de outro app. ~60 FPS estáveis
em 480×320 ampliados. PSS ~104–107 MiB. Não chamar de port ARM64 pronto.

Código HLE/SDL copiado do Ambitions; sua pasta não foi modificada. Keystore não foi copiada.
Proveniência em `docs/reference-source.json`. IPA, recursos extraídos, análise local,
saves, ambientes de desenvolvimento, chaves e builds são excluídos pelo `.gitignore`.

## Diferenças críticas

- Mach-O ARMv6 thin, sem PIE, base `0x1000`, sem relocações locais; loader reconstruído.
- `_main` `0xcb1f0`, Unix start `0x4b1c` (não chamar como função C).
- 480 imports, 1.245 relocs externas, 1.657 locais de bind, 43 classes, 28 construtores.
- ARM/Thumb misturados. Bit Thumb vem de `n_desc & 8` em símbolos locais.
- Literais PIC seguidos de ADD PC são deltas e não podem ser rebased.
- Switch `LDRcc PC,[PC,Rm,LSL #2]` tem destinos absolutos dentro de funções.
- **`__TEXT,__const` agora é preservada inteira**, não rebased. Contém constantes e strings;
  vtables ficam em `__DATA,__const`. `ANIMFACTOR_CRASHED` `0x126cb4=0xc000` (0,75 em 16.16)
  estava confundida com endereço de função, causando T-pose/tempos negativos no Sim exausto.
  Literal `0xe06dc=0x8840` é `GL_MATRIX_PALETTE_OES`; ponteiro real em `0x138b10` mantém rebase.
  Regressão confere seção inteira byte a byte. Agora 17.007 fixups internos, antes 17.022.
  `trace_animation=1` habilita hooks opcionais (normalmente desligados).
- ID `0x2741` não é o endereço ARM `0x2740` com bit Thumb. Regressões cobrem ambos.
- `loader/abi32.S`: realinhar pilha Apple4 → Android8 ao chamar HLE, preservando r0–r3
  e 128 bytes de argumentos. Sem isso dava SIGBUS em bionic malloc.
- CGFloat float; CFString 16 bytes; ObjC class_ro_t sem reserved; FILE._r offset4;
  fstat st_size offset60 (confirmado no código do jogo).
- Saves derivados de `files/Documents` são mapeados por fopen para `files/home/Documents`.

## Validação no aparelho

Os testes de diagnóstico e atualização foram realizados após salvar a partida.
O release 0.0.4 e o pacote UI estão instalados. Backup mais recente antes da atualização do ícone:
`analysis/icon-tests/home-before-icon/`. Backup do deploy HD anterior:
`analysis/graphics/device-before-hd-20261013-213515/home/`.
Logs/capturas em `analysis/ui-tests/`; animação em `analysis/pose-tests/fixed-constants/`.
Evitar reinícios/instalações durante uma sessão ativa. Use `adb devices -l` para identificar
o dispositivo de teste; seriais e endereços locais não são registrados nesta documentação.
Não alteramos configurações de energia/rede/root neste projeto.

Arquivo de save testado: `files/home/Documents/sim3dat0`, 2818 bytes, e `sim3set`.
Guardar antes de qualquer desinstalação. Atualizar com mesma assinatura via `adb install -r`.
`run_device.py` reinicia por padrão; **`--capture-only` observa sem reiniciar**.
Neste aparelho, keyevents ADB de Voltar/Home não acionaram os botões, mas tocar os botões
Android reais funcionou. Para abrir Tela via ADB durante sessão usar
`adb shell am start --activity-clear-top -a com.ts3wa.port.SCREEN_SETTINGS -n com.ts3wa.port/.LauncherActivity`.

## Próximos passos

1. Incorporar bugs que o usuário reportar; completar Sprint 3 (viagens, minijogos, áudio).
   Cenário do aeroporto aparece, mas viagem ainda não foi alcançada pelos toques ADB.
2. Validar importador IPA completo pelo seletor sem apagar home/saves.
3. ARM64-only: iniciar executor de CPU com guest ARM32 e ponte HLE de ponteiros/ABI.
   Unicorn 2.1.4 e Capstone 5.0.3 instalados no `.venv` próprio. **Prova no PC já funcionou**:
   `tools/emulate_boot.py analysis/sims3wa_armv6 --main` executa os 28 construtores e `_main`
   até UIApplicationMain/RuntimeAppDelegate em x86-64/64 bits. Não é APK ARM64 jogável.
   Ver `docs/05-EXECUCAO-EMULADA.md`; próxima etapa é core Unicorn ARM64 Android e ponte
   de objetos/structs/GL/callbacks guest32. Chamadas desconhecidas param com erro.
   Core e probe C ARM64 **já foram compilados** via `tools/build_unicorn.py` e são ELF64
   alinhados a 16 KiB em `analysis/executor-build/android-arm64/`. Probe Linux64 passou,
   retornando 20 de um método original M3G. Execução ARM64 no aparelho ainda pendente;
   próximo código é o guest loader/HLE C++ e integração no frontend Android64.
4. Assinatura definitiva/licença/distribuição comunitária quando o usuário decidir.

## Verificações

- `.venv/bin/python -m unittest discover -s tools/tests -v`: 13 testes passaram.
- Regressões C++ loader, GL (linkar `bitmap_hd.cpp`) e viewport passaram; comandos em `docs/03-COMPILAR.md`.
- Debug/release 0.0.3 compilados/instalados/testados. Release arquivado em `dist/` (1.550.804 bytes),
  assinatura v2/chave debug, APK sem assets. SHA-256
  `77e3e4665279c11cbb091027c5bd03bfaf8a2b0e506c98a3af42e060482faa0d`.
- Atualização 0.0.4 do ícone: release/lintVital/assinatura passaram; instalado e conferido no launcher.
- Saves preservados na atualização do ícone, com hashes comparados localmente ao backup.
  Os arquivos e seus identificadores particulares ficam fora do repositório.
- Último código nativo validado conserva a heurística de rebase experimental. Não generalizar
  para outros IPAs sem repetir análise e testes.

## Captura de logs

ADB USB estava conectado nas verificações da 0.0.3. Capturas são janelas finitas, não uma
gravação contínua da sessão do usuário. Verificar conexão antes de diagnosticar um novo bug.
