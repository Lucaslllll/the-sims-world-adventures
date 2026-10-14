# Arquitetura e estratégia

## Primeiro alvo: execução ARM32 no A10

```
IPA local → importador → Mach-O ARM32
                           ↓
                 loader legado + runtime ObjC2 ARM
                           ↓
                  HLE de frameworks iOS em C++
                           ↓
            SDL2 / GLES / OpenAL / Android / JNI
```

Usamos como base o código independente do port do Ambitions, importado por
`tools/bootstrap_reference.py`. A proveniência e os hashes anteriores às alterações
ficam em `docs/reference-source.json`. SDL2, OpenAL Soft e stb mantêm suas licenças
originais em `android/third_party/`.

O pacote `com.ts3wa.port` tem armazenamento e saves separados. A cópia de referência
é somente leitura e sua chave de assinatura não é importada.

## Adaptações implementadas

- Parser Mach-O32, symbol table, indirect imports, relocações ARM_RELOC_VANILLA.
- Rebase das referências internas de um executável não PIE.
- Seletores e estruturas Objective-C de 32 bits; trampolins em assembly ARM/Thumb.
- `CGFloat=float`, tipos dependentes do tamanho do ponteiro, strings constantes CF32.
- ABI de argumentos variádicos, estruturas retornadas por `objc_msgSend_stret` e
  alinhamento Apple ARMv6 versus Android AAPCS.
- Helpers numéricos `libgcc_s`, estruturas Darwin32 e APIs adicionais usadas nesta versão.

### Reconstrução de endereços no executável não PIE

O loader usa os símbolos retidos para distinguir regiões ARM e Thumb, encontra literais
LDR absolutos e preserva literais seguidos de ADD PC. Trata os destinos das tabelas
`LDRcc PC,[PC,Rm,LSL #2]` como endereços de código, mesmo quando estão no meio de funções.
Referências ObjC, CFStrings e tabelas C++ são ajustadas antes dos binds externos.

`__TEXT,__const` contém muitos dados numéricos empacotados. Nessa seção só são corrigidos
ponteiros exatos de callbacks de código conhecidos; candidatos que apenas se parecem
com endereços de dados são preservados. O processo ainda é experimental e específico
ao binário analisado, com regressões para os crashes realmente encontrados.

### Ponte de pilha ARMv6 → Android

`loader/abi32.cpp/.S` cria trampolins para funções importadas e métodos HLE. Preserva r0–r3,
copia até 128 bytes de argumentos da pilha original para uma pilha alinhada a 8 bytes,
chama o alvo ARM/Thumb e restaura a pilha do jogo. Métodos compilados no próprio jogo
continuam no ABI original. Sem a ponte, bionic causava SIGBUS dentro de `malloc`.
Isso não resolve automaticamente diferenças de packing de todos os argumentos de
64 bits; assinaturas adicionais precisam de validação quando forem efetivamente usadas.

### Arquivos e resolução

- Saves: `files/home/Documents/`. Paths antigos em `files/Documents/` passam por um
  redirecionamento em `fopen`, pois o jogo deriva esses caminhos do bundle.
- Bundle: `files/sims3wa_iphone.app/`, separado dos saves.
- Resolução lógica inicial: iPhone 480×320 (paisagem), ampliada com letterbox. É a
  resolução efetivamente testada, não 1520×720 renderizados pelo jogo.
- `CGFloat` é float; `FILE._r` fica no offset 4 e `stat.st_size` no offset 60.
- O nome do Sim volta pelo evento `EditingDidEndOnExit`, como na versão posterior.

## Alvo final: aparelhos exclusivamente 64-bit

Compilar a HLE para ARM64 não converte automaticamente o código ARM32 do jogo.
Precisaremos de um executor ARM32 (tradução dinâmica/emulação) e uma ponte de ABI,
incluindo callbacks, threads e memória de ponteiros de 32 bits. Isso deve ser tratado
como uma etapa própria e validada num dispositivo sem suporte ARM32.

O build `armeabi-v7a` é inicialmente uma ferramenta de pesquisa e validação, com
desempenho nativo em CPUs que ainda executam AArch32. O status público deve distinguir
esse alvo da compatibilidade final com Androids ARM64-only.
