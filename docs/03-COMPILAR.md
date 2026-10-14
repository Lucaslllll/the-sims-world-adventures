# Compilar e testar

## Ferramentas usadas

JDK 17, SDK Android 33, NDK 27.2.12479018, CMake 3.22.1, Gradle wrapper 8.10.2,
AGP 8.7.3. Dependências locais em `android/third_party/`: SDL2 2.30.9, OpenAL Soft
1.23.1 e stb_image. O scaffold já foi importado; não repetir o bootstrap num projeto alterado.

```bash
# a partir de android/
./gradlew assembleDebug --offline --max-workers=2 --console=plain
./gradlew assembleRelease --offline --max-workers=2 --console=plain
```

APK debug: `android/app/build/outputs/apk/debug/app-debug.apk`.
APK release: `android/app/build/outputs/apk/release/app-release.apk`.
Sem keystore própria, o release usa a chave de debug, conforme o scaffold. Não é uma
assinatura definitiva de distribuição; preservar a assinatura ao atualizar instalações.

## Validação no computador

```bash
python3 -m unittest discover -s tools/tests -v
g++ -std=c++20 -Itools/tests/include -Iandroid/app/src/main/cpp \
    tools/tests/loader_regression.cpp android/app/src/main/cpp/loader/macho.cpp \
    -o /tmp/ts3wa-loader-regression
/tmp/ts3wa-loader-regression analysis/sims3wa_armv6
```

São seis testes de análise, três testes opcionais do executor emulado e uma regressão
C++ contra o executável local. Use `.venv/bin/python` para executar também os emulados.
O teste C++
carrega/rebase/binda a imagem sem executar ARM no host. As resoluções de framework
desse teste são falsas deliberadamente; o teste cobre fixups, não a HLE ou o gameplay.

## ADB e dados locais

```bash
adb devices -l
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
# lançar uma vez para criar a pasta externa
adb shell am start -n com.ts3wa.port/.LauncherActivity
# verificar a pasta e copiar o bundle local, para desenvolvimento
adb shell ls /sdcard/Android/data/com.ts3wa.port/files
adb push extracted/Payload/sims3wa_iphone.app /sdcard/Android/data/com.ts3wa.port/files/
python3 tools/run_device.py --seconds 25
```

`run_device.py` reinicia o jogo e grava log/screenshot/crash em `analysis/device/`.
Use `--serial SEU_SERIAL_ADB` se houver mais de um aparelho. Obtenha o serial com
`adb devices -l`. Para observar uma sessão sem
reiniciar o jogo, use uma captura passiva:

```bash
adb logcat -T 1 -v threadtime -s TS3WA:* TS3WA-crash:* TS3WA-NSLog:* TS3WA-printf:*
python3 tools/run_device.py --capture-only --seconds 5
```

Saves: `/sdcard/Android/data/com.ts3wa.port/files/home/`. Faça backup antes de desinstalar
ou trocar a assinatura. Atualizações com a mesma assinatura usam `adb install -r`.
Não use `run_device.py` nem instale builds enquanto o usuário estiver validando uma partida.

## Artefato e evidências

```bash
python3 tools/archive_artifacts.py android/app/build/outputs/apk/release/app-release.apk --evidence
```

A ferramenta verifica o alvo ELF ARM32, ausência de arquivos em `assets/`, gera hash
SHA-256 e copia somente screenshots de jogo explicitamente selecionadas para `docs/img/`.
IPAs, análise, extração, saves, logs e APKs ficam fora do versionamento.

## Pesquisa de CPU emulada

```bash
python3 -m venv .venv
.venv/bin/pip install -r tools/requirements.txt
```

Esse ambiente é próprio do World Adventures; não modificar o venv do Ambitions.

```bash
.venv/bin/python tools/emulate_boot.py analysis/sims3wa_armv6 --main
.venv/bin/python -m unittest discover -s tools/tests -v
```

Ver [05-EXECUCAO-EMULADA.md](05-EXECUCAO-EMULADA.md) para os limites desse experimento.

## Gráficos HD

Build atual: 0.0.4, com `render_scale=2`, `texture_upscale=2`, `text_scale=2` e `screen_mode=fit` por padrão.
Esta revisão atualiza o ícone do launcher. Para regenerar os PNGs e as prévias a partir
dos vetores Android autorais: `.venv/bin/python tools/gen_icon.py`.
Para arquivar sem sobrescrever o artefato anterior:

```bash
python3 tools/archive_artifacts.py android/app/build/outputs/apk/release/app-release.apk \
    --version 0.0.4-arm32-hd-experimental
```

Regressão gráfica no computador:

```bash
g++ -std=c++20 -Itools/tests/include -Iandroid/app/src/main/cpp -Iandroid/third_party \
    tools/tests/graphics_regression.cpp android/app/src/main/cpp/hle/texture_pixels.cpp \
    android/app/src/main/cpp/hle/pvrtc.cpp android/app/src/main/cpp/hle/bitmap_hd.cpp \
    -ldl -pthread -o /tmp/ts3wa-graphics-regression
/tmp/ts3wa-graphics-regression
g++ -std=c++20 -Iandroid/app/src/main/cpp tools/tests/viewport_regression.cpp \
    -o /tmp/ts3wa-viewport-regression
/tmp/ts3wa-viewport-regression
```

O venv com `tools/requirements-graphics.txt` também executa quatro testes gráficos,
totalizando 13 testes Python. Extração, IA, pacote e deploy em [06-GRAFICOS-HD.md](06-GRAFICOS-HD.md).
