# Análise do binário — World Adventures 1.0.0

## Evidências da cópia local

| Item | Valor |
|---|---|
| Bundle | `Payload/sims3wa_iphone.app/` |
| Identificador iOS | `com.ea.sims3wa.bv` |
| Executável | `sims3wa_iphone`, 2.517.728 bytes |
| CPU | ARM, subtipo 6 (ARMv6), 32-bit, Mach-O thin |
| SDK / mínimo | iPhone OS 3.0 / 2.2.1 |
| FairPlay | `LC_ENCRYPTION_INFO.cryptid = 0` |
| PIE | Não (`MH_PIE` ausente) |
| Entrada Unix | `start` em `0x4b1c` |
| Função C | `_main` em `0xcb1f0` |
| Imports reais | 480 (`LC_DYSYMTAB`, não contar símbolos STABS como imports) |
| Relocações externas | 1.245, ARM_RELOC_VANILLA, 32-bit, não PC-relative |
| Relocações locais | Nenhuma |
| Symbols | 36.161 entradas, incluindo informação de depuração STABS |
| Objective-C | ObjC2, 43 classes, 796 referências a seletores |
| Bundle | 1.525 arquivos, aproximadamente 127,4 MiB |

Segmentos:

```
__PAGEZERO  0x00000000  tamanho 0x001000
__TEXT      0x00001000  tamanho 0x137000  R-X
__DATA      0x00138000  tamanho 0x034000  RW-
__LINKEDIT  0x0016c000  tamanho 0x11f000  R--
```

O entrypoint Unix espera `argc/argv/envp` na pilha, como o dyld antigo. Não pode ser
chamado como uma função C. O loader usará o símbolo `_main` e chamará antes os 28
construtores em `__mod_init_func`.

## Diferenças em relação ao Ambitions

| Área | Ambitions | World Adventures |
|---|---|---|
| CPU | ARM64 | ARMv6/ARM32 |
| Fixups | rebase/bind via `LC_DYLD_INFO_ONLY` | relocação externa e indirect symbol table |
| Entrada | `LC_MAIN` | `LC_UNIXTHREAD` + símbolo `_main` |
| Ponteiros | 8 bytes | 4 bytes |
| `CGFloat` | double | float |
| ObjC mensagens | x0/x1 + FP regs | r0/r1; `objc_msgSend_stret` separado |
| ObjC `class_ro_t` | campo `reserved` após instanceSize | sem esse campo |
| Strings CF constantes | 32 bytes | 16 bytes |
| Varargs | todos na pilha, slots de 8 | r0–r3 + pilha, slots de 4; investigar alinhamento de 64-bit |
| `FILE` / `stat` / CRC | layout Darwin64 | layouts Darwin32 e CRC de 32 bits |
| Helpers numéricos | operações ARM64 | dezenas de imports `libgcc_s` com sufixo `vfp` |

## Endereços fixos

O Android do A10 tem `vm.mmap_min_addr=32768`; `__TEXT` começa em 4096.
Mapear o jogo no endereço original não é uma solução portátil. Não alteramos essa
configuração do aparelho. O protótipo deverá reconstruir referências internas ao
deslizar a imagem. A falta de relocações locais exige cuidado com pools de literais ARM,
ponteiros em metadados ObjC e tabelas C++. Qualquer rebase heurístico será marcado como
experimental, com validação de menu e gameplay ainda necessária.

## Recursos

O bundle contém PNGs, modelos `.m3g`, WAVs, CAFs, dados `.bin/.dat`, TIFFs e strings
localizadas. O Info.plist declara en, fr_FR, it, de, es_ES e ja.
Não presumir suporte a português apenas porque a HLE do Ambitions oferece esse idioma.

## Reprodução

```bash
python3 tools/analyze_ipa.py The_Sims_3_World_Adventures__International__1.0.0_ios_2.2.1.ipa --extract extracted
```

O relatório completo fica em `analysis/report.json` e os imports em `analysis/imports.txt`.
Reexecutar sem `--extract` para atualizar só a análise; a extração preserva arquivos existentes.
