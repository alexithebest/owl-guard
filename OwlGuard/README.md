# OWL GUARD

Anti Discord token grabber. Watchdog em userland + shield ring-0.

```
OPEN SORCES FREE JOIN https://discord.gg/MMtdav2EJy FOR MORE!
```

## O que ele faz

| camada | tecnica | detecta / bloqueia |
|---|---|---|
| process watch | snapshot diff de `CreateToolhelp32Snapshot` | familias conhecidas (BlankGrabber, RedLine, Lumma, Vidar, Stealc, Raccoon...), binario sem assinatura em AppData/Temp, padrao de token Discord, strings de coleta |
| handle watch | `NtQuerySystemInformation(SystemExtendedHandleInformation)` + `DuplicateHandle` | qualquer processo com `PROCESS_VM_READ` contra o PID do Discord |
| file watch | `ReadDirectoryChangesW` + Restart Manager | acesso ao `Local Storage\leveldb` do Discord e a `Login Data` / `Local State` / cookies dos navegadores |
| driver ring-0 | `ObRegisterCallbacks` + process notify | remove `VM_READ / VM_WRITE / VM_OPERATION / CREATE_THREAD` dos handles abertos contra o Discord. e o bloqueio de verdade |

O driver protege o Discord sozinho a partir do boot — nao precisa do GUI aberto.

## Build — app

Precisa de Visual Studio 2022 (C++ desktop) e CMake 3.20+.

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Saida: `build\Release\OwlGuard.exe`.

## Build — driver

1. Visual Studio 2022 + WDK (Windows Driver Kit), ambos instalados.
2. Novo projeto **Empty WDM Driver** (x64).
3. Adicione `driver\antigrab.c` e `src\shared.h`.
4. Propriedades do projeto:
   - `C/C++ > General > Treat Warnings as Errors` = No
   - `Driver Signing > Sign Mode` = Test Sign (para laboratorio)
5. Build Release. Copie `OwlGuard.sys` para a mesma pasta do `.exe`.

Assinatura: driver nao assinado nao carrega no Windows 11 normal.
- Laboratorio: `bcdedit /set testsigning on` e reinicie (marca d'agua no canto).
- Producao: certificado EV + atestacao Microsoft (Partner Center).

O app instala e sobe o servico automaticamente quando roda como administrador
e encontra `OwlGuard.sys` ao lado do executavel.

Manual:

```bat
sc create OwlGuard type= kernel start= system binPath= "C:\caminho\OwlGuard.sys"
sc start OwlGuard
```

## Inicializacao — o que roda quando

| mecanismo | quando inicia | elevado |
|---|---|---|
| `HKCU\...\Run` | logon do usuario | nao |
| tarefa agendada (usada se admin) | logon do usuario | sim, sem UAC |
| driver `start= system` | boot, antes de qualquer logon | kernel |
| driver `start= boot` | carregado pelo boot loader, o mais cedo possivel | kernel |

`start= boot` (o "antes do boot" real) e um comando:

```bat
sc config OwlGuard start= boot
```

Use com cuidado: driver boot-start com bug derruba o boot. Teste em VM primeiro.
O GUI nunca roda antes do logon — o escudo ring-0 sim.

## Uso

1. Marque **Iniciar com o Windows** (obrigatorio para ativar).
2. Escolha os modulos.
3. **ATIVAR PROTECAO**.

A caixa de inicializacao e o gate: sem ela o botao fica inerte.
`--autostart` (usado pela tarefa agendada) sobe o motor sozinho.

## Halo roxo + selo dentro do Discord

Com a proteção ativa, duas janelas topmost click-through grudam na janela do Discord:

- **halo roxo** — moldura com per-pixel alpha (`UpdateLayeredWindow` + SDF arredondado), gradiente `#7C3AED > #A855F7`, 18 px de glow.
- **selo** — pílula roxa no canto inferior direito com `OWL GUARD PROTECTING YOU!`, `WS_EX_TRANSPARENT`, não rouba clique nem foco.

Nada é injetado no cliente. Funciona em qualquer versão, sem plugin. Some quando a proteção para ou o Discord fecha.

Preview sem ligar o motor:

```bat
OwlGuard.exe --overlay-demo
```

### Roxo dentro da UI (opcional)

`theme\owl-purple.theme.css` recolore o Discord inteiro e coloca o selo via CSS.
Requer Vencord:

1. Vencord > Themes > **Open Themes Folder**
2. copie `owl-purple.theme.css` para a pasta
3. ative em Themes

## Estrutura

```
CMakeLists.txt
src/main.cpp      GUI GDI, tema escuro, sem dependencias externas
src/guard.cpp     motor: processos, handles, arquivos, driver, autostart
src/overlay.cpp   halo roxo + selo sobre a janela do Discord
src/overlay.h
src/guard.h       API do motor
src/shared.h      contrato app <-> driver (IOCTLs)
src/app.manifest  DPI per-monitor, asInvoker
driver/antigrab.c shield ring-0
theme/owl-purple.theme.css  tema Vencord (roxo + selo)
```
