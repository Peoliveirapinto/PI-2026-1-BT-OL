# Sobreposição de Bateria Bluetooth

Uma pequena sobreposição Win32 que fica sobre a área de trabalho e mostra a porcentagem de bateria, o tempo estimado restante e uma estimativa histórica de "saúde" para dispositivos Bluetooth LE que expõem o serviço padrão de Bateria.

## Funcionalidades

- Consulta dispositivos Bluetooth LE em uma thread de segundo plano.
- Lê a porcentagem da bateria através do serviço GATT Battery (`0x180F`) e da característica Battery Level (`0x2A19`).
- Rastreia o histórico de descarga por dispositivo e estima o tempo restante.
- Estima a "saúde" com base no comportamento de descarga observado quando o dispositivo não fornece uma métrica direta de integridade.
- Renderiza por meio de uma janela Win32 em camadas com click-through, mantendo a thread de interface leve.

## Requisitos

- **CMake** 3.15 ou posterior
- **Visual Studio 18** (MSVC) ou compilador C++ equivalente
- Windows 10 ou superior

## Compilação

Use o CMake com um toolchain do Windows:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Observações

- A sobreposição é intencionalmente minimalista e otimizada para uso em tela ociosa.
- Atalhos fáceis para fechar: Ctrl+Shift+Q e Ctrl+Alt+Q.
- Alternar em tempo de execução: Ctrl+Alt+T ativa/desativa o click-through sem reiniciar.
- Menu do ícone na bandeja do sistema: clique com o botão direito para "Recarregar agora" e "Sair".
- A implementação atual tem como alvo dispositivos Bluetooth LE que expõem o serviço GATT de bateria padrão. Esse é o caminho nativo mais confiável no Windows sem adicionar dependências de runtime mais pesadas.

## Modo de depuração

- Execute com `--debug` para desabilitar o click-through e habilitar uma janela normal com botão de fechar.
- Execute com `--console` para abrir um console de depuração e imprimir logs de atualização.
- Diagnósticos por dispositivo são impressos nos logs do console de depuração (etapa de descoberta e motivo da leitura da bateria).
- Na sobreposição, diagnósticos são mostrados em cada linha quando a bateria não está disponível ou quando executado com `--debug`.
- Ordem de busca da bateria: serviço GATT de bateria LE -> fallback para propriedade do dispositivo Bluetooth do Windows.
- Se as chaves padrão estiverem vazias, o aplicativo realiza um fallback por varredura de chaves de propriedade numéricas e reporta a chave vencedora nos diagnósticos.
- O fallback por endereço de dispositivos relacionados agora usa pontuação de confiança; valores fracos da varredura de chaves são registrados como candidatos, mas não são aplicados automaticamente.

Exemplos:

```powershell
build/windows-x64/Release/BluetoothBatteryOverlay.exe --debug
build/windows-x64/Release/BluetoothBatteryOverlay.exe --debug --console
```
