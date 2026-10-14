# Guia experimental do jogador

## Compatibilidade atual

- Android 8 ou posterior **com suporte a aplicativos ARM32 (`armeabi-v7a`)**.
- Testado no Galaxy A10, Android 10, Mali-G71. Outros aparelhos ainda precisam de teste.
- Celulares exclusivamente 64-bit ainda não são atendidos por este APK.
- IPA local World Adventures 1.0.0, Mach-O ARMv6 descriptografado, fornecido pelo jogador.

## Instalar

1. Instale `dist/TS3WorldAdventures-0.0.4-arm32-hd-experimental.apk`.
2. Abra **TS3 World Adventures** e selecione a sua cópia `.ipa` em Downloads.
3. Aguarde a importação (~128 MiB extraídos) e o carregamento inicial.

O fluxo completo do seletor/importador desta adaptação ainda precisa de validação no
aparelho. Nos testes de desenvolvimento, o bundle foi copiado pelo ADB. O APK não contém
o executável nem os recursos do jogo.

## Controles e saves

- Toque nos objetos/terreno, arraste a câmera e use o slider de zoom do próprio jogo.
- O nome do Sim é digitado num diálogo com teclado Android.
- Para salvar: menu `...` → **Save and Continue** → confirmação verde.
- Salve antes de sair. O salvamento manual e o carregamento após reiniciar foram testados.
- O perfil atual renderiza em 960×640 mantendo o layout original; há barras laterais
  em telas largas no modo padrão. A fonte é rasterizada em 2×. O pacote AI 2× é opcional
  e instalado separadamente pelo computador.
- Para preencher a tela: **Voltar do Android → Tela → Preencher a tela**. A imagem é
  esticada, mantendo todos os controles; os personagens e botões ficam mais largos.
  **Proporção original (3:2)** restaura o padrão. A escolha fica salva.
- Em launchers compatíveis, manter pressionado o ícone do app também oferece **Tela**.
- Sem idioma suportado no IPA, a seleção inicial cai para inglês. O IPA analisado tem
  inglês, francês, italiano, alemão, espanhol e japonês.

## Estado conhecido

Menu, CAS, casa, cidade e uma interação com o sofá funcionaram. Viagens, missões,
minijogos, importação/exportação de Sims, mais idiomas, sessão longa e áudio ainda estão
em validação. APIs antigas de serviços da EA e StoreKit estão parcialmente representadas
por placeholders; o log mostra os seletores usados sem implementação.

O pacote HD local ocupa ~272 MiB e contém 979 imagens ampliadas. Sem ele, o perfil 2×
usa renderização maior e upscale filtrado das texturas. Instalação e comparação em
[06-GRAFICOS-HD.md](06-GRAFICOS-HD.md). O A10 de teste já está com o pacote instalado.

Na versão 0.0.3, o pacote `dist/TS3WorldAdventures-HD2x-UI-0.0.3-local.zip` refina os ícones
mantendo as demais texturas do cenário. A correção de constantes de animação também
resolveu a T-pose observada no Sim exausto; detalhes em [SPRINTS.md](SPRINTS.md).

Se houver crash, o relatório fica em `files/crash.txt` no armazenamento do aplicativo.
Informe a ação, a tela e se ocorreu novamente após carregar o save; isso permite ligar o
erro ao endereço no executável original. O nome e os saves ficam fora do bundle importado.
