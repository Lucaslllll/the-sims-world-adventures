# Execução ARM32 em host 64-bit — experimento de CPU

## Resultado reproduzido em 2026-10-12

`tools/emulate_boot.py` executou **os 28 construtores originais e o `_main` do jogo**
até a chamada `UIApplicationMain(argc=1, delegate="RuntimeAppDelegate")` em um processo
Python **x86-64 de 64 bits**. A CPU guest é ARM32, incluindo código ARM e Thumb.

- Backend: Unicorn 2.1.4, executor derivado do QEMU.
- Executável: SHA-256 `01d3625643f01d458e754987ad9f0fbcd8841d89f606f16e59215daaa14cfe15`.
- Base guest original: `0x1000`. Não precisa de rebase interno nem de alterar
  `vm.mmap_min_addr`, porque esse endereço pertence à memória virtual do emulador.
- Ponteiros guest mantêm 32 bits. Nenhum ponteiro Python/host é escrito no Mach-O.
- Foi executada também a categoria **UIDevice(Hardware)** do próprio jogo: a resolução
  de métodos redireciona o PC guest para o IMP original; seu `platform` chama `sysctlbyname`.
- A ordem de grandeza da execução inicial foi ~0,12 s no PC, medida incluindo preparo
  do guest. Não é medição de FPS, consumo de bateria ou desempenho num Android ARM64.

Relatório completo em `analysis/emulated-boot.json`. O experimento **não renderiza**,
não implementa UIKit, não contém um APK ARM64 e não comprova gameplay em ARM64-only.

## Reproduzir

```bash
.venv/bin/pip install -r tools/requirements.txt
.venv/bin/python tools/emulate_boot.py analysis/sims3wa_armv6 --main
.venv/bin/python -m unittest discover -s tools/tests -v
```

Sem `--main`, a execução termina após os construtores. Sem uma dependência ou o IPA local,
os testes emulados são pulados explicitamente; o ambiente de pesquisa executa todos.

## APIs do experimento

Existem implementações mínimas, limitadas ao startup, para:

- Alocação numa arena guest, new/delete, memcpy/memset e contadores atômicos.
- CFArray/CFString e lista de idiomas; strings constantes usam o layout original CF32.
- Objetos NSAutoreleasePool, NSLock/NSRecursiveLock e UIDevice.
- `sysctlbyname` reportando iPhone 3GS, helpers numéricos Darwin e guards C++.
- Métodos de NSString realmente alcançados pelo startup e mensagens à categoria original.

A arena mantém alocações até o fim do processo; locks são modelos de execução serial;
registros SJLJ e destructores `atexit` são ignorados neste experimento curto sem exceções.
Imports de dados não usados têm blocos guest distintos marcados como placeholders no
relatório. Heap e pilha não são executáveis: saltar para um placeholder é uma falha.

**Chamadas sem handler e seletores desconhecidos interrompem a execução com erro.**
O teste negativo remove uma implementação necessária e verifica que o experimento
não registra falsamente um boot completo. Um teste adicional cobre retorno double zero
em r0:r1, evitando que o segundo registrador retenha um argumento antigo.

## Caminho para o Android ARM64

1. Compilar e validar o core Unicorn para Android `arm64-v8a`, sem usar o processo ARM32.
2. Transportar o guest loader e a resolução de imports do protótipo para C/C++.
3. Criar representações de objetos/structs iOS em memória **guest de 32 bits**. O runtime
   ObjC nativo atual usa ponteiros C++ e não pode ser simplesmente ligado a esse guest.
4. Adaptar as APIs gráficas, áudio e entrada já pesquisadas: converter structs e buffers
   guest, invocar GLES/Android no host e devolver resultados no ABI ARMv6.
5. Reentrar na CPU guest em callbacks, timers e threads; documentar posse do contexto GL
   e a política de execução concorrente. Unicorn não fornece frameworks iOS ou JNI.
6. Medir cenas reais num aparelho ARM64-only. Começar pelo menu, CAS, mundo e saves,
   repetir os casos da versão nativa e só então avaliar desempenho geral.

O sucesso de startup reduz o risco de suporte a instruções e de carregamento do Mach-O.
O trabalho restante concentra-se na ponte guest/host e nas APIs, não numa conversão
automática do código ARMv6 para ARM64.

## Núcleo Android ARM64 compilado

O experimento C em `experiments/arm64-executor/` e o core Unicorn foram compilados no
computador com NDK r27c para **`arm64-v8a`, API 26**. Os dois artefatos são ELF64/AArch64;
os segmentos LOAD estão alinhados a 16 KiB e o executável usa `/system/bin/linker64`.

```bash
# Compilar o core e o probe Android; não faz nenhuma chamada ADB
python3 tools/build_unicorn.py --jobs 2
# Compilar e executar o probe equivalente no PC Linux 64-bit
python3 tools/build_unicorn.py --host --jobs 2
```

O probe C lê da cópia local as instruções de um método original de M3G VertexArray
em `0xdf3a0`, executa-as no guest ARM32 e verifica o retorno do ID de classe 20.
A versão Linux 64-bit passou. A versão Android ARM64 foi **compilada e inspecionada**,
mas ainda não executada no aparelho: o usuário está validando o jogo instalado.
Esse teste C é menor que o startup completo do protótipo Python.

Artefatos:

- `analysis/executor-build/android-arm64/ts3wa_cpu_probe`
- `analysis/executor-build/android-arm64/unicorn/libunicorn.so`
- `analysis/executor-build/android-arm64/build-report.json`
- `analysis/executor-build/host64/build-report.json`

O builder fixa versão e SHA-256 do tarball de fonte:
`ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4`.
O código de terceiros conserva o `COPYING` GPLv2 no pacote original baixado.
Nenhum código ou asset da EA é incorporado ao probe ou à biblioteca.

Próxima implementação: portar o guest loader e a ponte de APIs do Python para um
runtime C/C++ com memória guest32 estável, sem misturar ponteiros nativos de 64 bits.

## Referências consultadas

- Unicorn: <https://www.unicorn-engine.org/docs/>
- Compilação: <https://github.com/unicorn-engine/unicorn/blob/master/docs/COMPILE.md>
- Helpers Darwin VFP (comparações retornam booleanos, não o resultado genérico libgcc):
  <https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/builtins/arm/eqsf2vfp.S>
  e `gesf2vfp.S` no mesmo diretório.
