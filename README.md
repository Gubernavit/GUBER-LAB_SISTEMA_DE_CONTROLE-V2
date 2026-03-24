# GUBER-LAB — Sistema de Controle de Relés via ESP32 + Dashboard Web

https://guberlab-sistemadecontrole.web.app/

Dashboard web + firmware ESP32 para controle remoto de 8 relés em tempo real, usando MQTT, Firebase e EmailJS.

---

## Visão geral

```
[ Dashboard Web (GitHub Pages) ]
          ↕ MQTT (HiveMQ público)
     [ ESP32 + 8 Relés ]
```

O dashboard roda no navegador (hospedado no GitHub Pages — sem servidor próprio). O ESP32 se conecta ao mesmo broker MQTT público e responde a comandos em tempo real.

---

## O que está incluído

### `index.html` — Dashboard Web
- Login com Firebase Auth + modo demo (`teste / teste123`)
- Controle MQTT dos 8 relés em tempo real
- Sistema de usuários com 4 níveis (Admin / Operador / Visualizador / Convidado)
- Histórico completo de ações (quem fez o quê e quando)
- Automações por horário e por cadeia de eventos
- Relatório diário por e-mail (via EmailJS)
- Renomear cada canal individualmente
- Design responsivo (mobile / tablet / desktop)

### `guberlab_firmware.ino` — Firmware ESP32
- Controle físico por botão (toque rápido = troca canal, toque longo = liga/desliga)
- LED RGB com feedback visual por estado
- Buzzer com sons distintos para cada ação
- Conexão WiFi automática com reconexão
- Portal de configuração via AP (caso o WiFi falhe)
- Configurações salvas na flash (persistem após reinicialização)
- Comunicação MQTT com o dashboard

---

## Hardware necessário

- ESP32 (qualquer variante com WiFi)
- Módulo de 8 relés (ativo em LOW)
- 1 botão (entre GPIO 4 e GND)
- 1 LED RGB de catodo comum (ou 3 LEDs individuais)
- 1 buzzer passivo
- Fonte 5 V para os relés (dependendo da carga)

### Pinagem do ESP32

| Pino    | Função               |
|---------|----------------------|
| GPIO 4  | Botão de controle    |
| GPIO 2  | Buzzer               |
| GPIO 13 | LED RGB — Vermelho   |
| GPIO 12 | LED RGB — Verde      |
| GPIO 14 | LED RGB — Azul       |
| GPIO 16 | Relé 1               |
| GPIO 17 | Relé 2               |
| GPIO 18 | Relé 3               |
| GPIO 19 | Relé 4               |
| GPIO 21 | Relé 5               |
| GPIO 22 | Relé 6               |
| GPIO 23 | Relé 7               |
| GPIO 25 | Relé 8               |

> O módulo de relé utilizado é **ativo em LOW** (HIGH = desligado, LOW = ativado).  
> Ajuste o array `RELAY_PINS[]` no firmware caso sua fiação seja diferente.

---

## Comportamento do LED RGB

| Cor                      | Situação                                     |
|--------------------------|----------------------------------------------|
| 🟠 Laranja piscando rápido | Tentando conectar ao WiFi                  |
| 🔵 Azul (60%)             | Conectado e operacional                      |
| 🟠 Laranja piscando lento  | Modo AP — portal de configuração ativo      |
| 🟠 Laranja flash           | Troca de canal / confirmação de toggle      |
| 🟢 Verde                  | Canal selecionado está **ligado**            |
| 🔴 Vermelho               | Canal selecionado está **desligado**         |

## Comportamento do Buzzer

| Som                        | Situação                        |
|----------------------------|---------------------------------|
| 2 pulsos curtos            | Iniciando / tentando conectar   |
| Melodia ascendente (3 notas) | WiFi conectado com sucesso    |
| Beep único curto           | Troca de canal                  |
| 2 notas subindo            | Relé **ligado**                 |
| 2 notas descendo           | Relé **desligado**              |

## Botão — Lógica de controle

| Ação                   | Resultado                             |
|------------------------|---------------------------------------|
| Toque rápido (< 600 ms) | Avança para o próximo canal (1→2→…→8→1) |
| Toque longo (≥ 600 ms)  | Liga ou desliga o canal selecionado   |

---

## Configuração — passo a passo

### Pré-requisitos

- Conta Google (para Firebase)
- Conta gratuita no [EmailJS](https://www.emailjs.com) (para relatórios por e-mail)
- Conta no [GitHub](https://github.com) (para hospedar o dashboard)
- Arduino IDE com suporte ao ESP32

---

### PASSO 1 — Criar projeto no Firebase

1. Acesse [console.firebase.google.com](https://console.firebase.google.com)
2. Clique em **"Adicionar projeto"** → dê qualquer nome (ex: `guberlab`) → clique **Criar projeto**

#### 1.1 — Ativar Authentication

1. No menu lateral: **Authentication → Começar**
2. Em **Sign-in method**, ative **E-mail/senha**

#### 1.2 — Ativar Realtime Database

1. No menu lateral: **Realtime Database → Criar banco de dados**
2. Escolha qualquer região → selecione **modo de teste** por enquanto
3. Após criar, vá em **Regras** e substitua o conteúdo por:

```json
{
  "rules": {
    "users": {
      "$uid": {
        ".read": "$uid === auth.uid",
        ".write": "auth != null && (root.child('users').child(auth.uid).child('role').val() === 'admin' || $uid === auth.uid)"
      }
    },
    "history": {
      ".read": "auth != null",
      ".write": "auth != null",
      ".indexOn": ["ts"]
    },
    "automations": {
      ".read": "auth != null",
      ".write": "auth != null && root.child('users').child(auth.uid).child('role').val() === 'admin'"
    },
    "relay_names": {
      ".read": "auth != null",
      ".write": "auth != null && root.child('users').child(auth.uid).child('role').val() === 'admin'"
    },
    "settings": {
      "$uid": {
        ".read": "$uid === auth.uid",
        ".write": "$uid === auth.uid"
      }
    }
  }
}
```

4. Clique **Publicar**

#### 1.3 — Registrar o App Web

1. Na tela inicial do projeto, clique em **`</>`** (Web)
2. Dê um apelido (ex: `guberlab-web`) → clique **Registrar app**
3. Copie o objeto `firebaseConfig` que aparecer. Ele terá este formato:

```js
const firebaseConfig = {
  apiKey:            "AIzaSy...",
  authDomain:        "nome-do-projeto.firebaseapp.com",
  databaseURL:       "https://nome-do-projeto-default-rtdb.firebaseio.com",
  projectId:         "nome-do-projeto",
  storageBucket:     "nome-do-projeto.firebasestorage.app",
  messagingSenderId: "000000000000",
  appId:             "1:000000000000:web:abcdefabcdef"
};
```

4. No arquivo `index.html`, localize o bloco marcado com o comentário `// CONFIGURAÇÃO FIREBASE` e substitua os valores pelos seus.

---

### PASSO 2 — Criar o primeiro usuário Admin

1. No Firebase: **Authentication → Usuários → Adicionar usuário**
2. Preencha e-mail e senha (ex: `admin@seudominio.com` / `suaSenhaForte123`)
3. Copie o **UID** gerado (coluna "Identificador de usuário")
4. Vá em **Realtime Database → Dados**
5. Crie a seguinte estrutura (clicando com o botão `+`):

```
users/
  [UID copiado]/
    name:  "Seu Nome"
    email: "admin@seudominio.com"
    role:  "admin"
```

Pronto! Agora você consegue logar no dashboard com este e-mail e senha.

> **Dica:** o campo `email` no login aceita tanto o e-mail completo quanto apenas o nome antes do `@guberlab.local` — basta digitar `admin` e o sistema completa automaticamente. Para domínios personalizados, use o e-mail completo.

---

### PASSO 3 — Configurar EmailJS (relatórios diários por e-mail)

1. Crie conta gratuita em [emailjs.com](https://www.emailjs.com)
2. Em **Email Services**, conecte seu Gmail ou outro provedor
3. Em **Email Templates**, crie um template e use estas variáveis no corpo:

| Variável       | Conteúdo                     |
|----------------|------------------------------|
| `{{to_email}}` | Destinatário                 |
| `{{subject}}`  | Assunto do relatório         |
| `{{message}}`  | Corpo com o histórico do dia |
| `{{date}}`     | Data do relatório            |

4. Anote:
   - **Service ID** (em Email Services)
   - **Template ID** (em Email Templates)
   - **Public Key** (em Account → General)

5. No `index.html`, localize o bloco `// CONFIGURAÇÃO EMAILJS` e preencha:

```js
const EMAILJS_SERVICE  = 'seu_service_id';
const EMAILJS_TEMPLATE = 'seu_template_id';
const EMAILJS_KEY      = 'sua_public_key';
let   REPORT_EMAIL     = 'seu@email.com';
const REPORT_HOUR      = 20; // hora do envio (20 = 20:00)
```

6. Copie os mesmos valores para o firmware (`guberlab_firmware.ino`), nas linhas marcadas com `// EMAILJS`.

---

### PASSO 4 — Configurar e gravar o firmware

#### 4.1 — Instalar dependências no Arduino IDE

No **Library Manager** (Ctrl+Shift+I), instale:

| Biblioteca    | Autor               |
|---------------|---------------------|
| PubSubClient  | Nick O'Leary        |
| ArduinoJson   | Benoit Blanchon (v6+) |
| WiFiManager   | tzapu / tablatronix |

> As bibliotecas `WiFi.h`, `HTTPClient.h` e `time.h` já vêm incluídas no core do ESP32 — não precisam ser instaladas.

#### 4.2 — Editar credenciais no firmware

Abra `guberlab_firmware.ino` e edite as linhas marcadas com os comentários:

```cpp
// ── WIFI ──────────────────────────────────────────────────────────────────────
#define WIFI_SSID    "NOME_DA_SUA_REDE"
#define WIFI_PASS    "SENHA_DA_SUA_REDE"

// ── EMAILJS ───────────────────────────────────────────────────────────────────
#define EMAILJS_SERVICE  "seu_service_id"
#define EMAILJS_TEMPLATE "seu_template_id"
#define EMAILJS_KEY      "sua_public_key"
#define EMAILJS_TO       "seu@email.com"
```

> **Se preferir não colocar o WiFi no código:** deixe `WIFI_SSID` e `WIFI_PASS` em branco — o ESP32 abrirá automaticamente o portal de configuração (veja abaixo).

#### 4.3 — Gravar no ESP32

1. No Arduino IDE, selecione a placa **ESP32 Dev Module** (ou equivalente)
2. Selecione a porta COM correta
3. Clique em **Upload**

---

### PASSO 5 — Portal de configuração WiFi (modo AP)

Se o WiFi não conectar (senha errada, rede fora do ar, etc.), o ESP32 entra automaticamente no **modo AP**:

| Campo    | Valor padrão        |
|----------|---------------------|
| Rede AP  | `GuberLab-Config`   |
| IP       | `192.168.4.1`       |
| Usuário  | `admin`             |
| Senha    | `admin123`          |

> **Recomendado:** altere o usuário e a senha do portal nas linhas `PORTAL_USER` e `PORTAL_PASS` do firmware antes de gravar.

No portal você pode reconfigurar SSID, senha e resetar para os padrões de fábrica. As configurações são salvas na flash e persistem após reinicialização.

---

### PASSO 6 — Hospedar o dashboard no GitHub Pages

1. Crie um repositório público no GitHub (ex: `guberlab`)
2. Faça upload do arquivo `index.html` (com suas credenciais já preenchidas)
3. Vá em **Settings → Pages**
4. Em **Source**, selecione a branch `main` e pasta `/root`
5. Clique **Save**
6. Aguarde 1–2 minutos e acesse: `https://SEU_USUARIO.github.io/guberlab`

> ⚠️ **Segurança:** como o `index.html` ficará público, as credenciais do Firebase ficarão visíveis no código. Isso é esperado e seguro desde que as **Regras do Firebase** exijam autenticação (como configurado no Passo 1.2). Nunca coloque credenciais de admin diretamente no HTML.

---

## Protocolo MQTT

O dashboard e o ESP32 se comunicam via broker público **HiveMQ** (`broker.hivemq.com:1883`).

| Tópico              | Direção               | Descrição                       |
|---------------------|-----------------------|---------------------------------|
| `guberlab/comando`  | Dashboard → ESP32     | Envia comandos                  |
| `guberlab/status`   | ESP32 → Dashboard     | Retorna estados                 |

> Para uso em produção, considere trocar para um broker privado e alterar `MQTT_HOST` no firmware.

### Comandos aceitos pelo ESP32

| Comando        | Ação                              |
|----------------|-----------------------------------|
| `PING`         | ESP32 responde `PONG`             |
| `STATUS`       | ESP32 publica o estado de todos os canais |
| `T1` a `T8`    | Toggle do canal (inverte estado)  |
| `L1` a `L8`    | Liga canal específico             |
| `D1` a `D8`    | Desliga canal específico          |
| `L_TUDO`       | Liga todos os canais              |
| `D_TUDO`       | Desliga todos os canais           |

### Respostas publicadas pelo ESP32

| Mensagem | Significado            |
|----------|------------------------|
| `PONG`   | Confirmação de ping    |
| `S1:1`   | Canal 1 **ligado**     |
| `S3:0`   | Canal 3 **desligado**  |

---

## Níveis de acesso

| Nível         | Operar Relés | Ver Histórico | Automações | Usuários |
|---------------|:---:|:---:|:---:|:---:|
| Admin         | ✅  | ✅  | ✅  | ✅  |
| Operador      | ✅  | ✅  | ❌  | ❌  |
| Visualizador  | ❌  | ✅  | ❌  | ❌  |
| Convidado     | 🔵 Simulado | 🔵 Demo | 🔵 Demo | 🔵 Demo |

Usuários são criados pelo Admin diretamente no dashboard (aba **Usuários**). O sistema cria o usuário no Firebase Auth e salva as permissões no Realtime Database automaticamente.

---

## Credenciais padrão incluídas no código

| Usuário   | Senha      | Onde                          | Função                              |
|-----------|------------|-------------------------------|-------------------------------------|
| `teste`   | `teste123` | Dashboard (hardcoded)         | Modo demo — vê tudo, não executa nada |
| `admin`   | `admin123` | Portal AP do ESP32            | Acesso ao portal de configuração WiFi |

> O usuário `teste` é apenas demonstração local — não existe no Firebase.  
> Altere a senha do portal AP em `PORTAL_USER` / `PORTAL_PASS` no firmware.

---

## Estrutura dos arquivos

```
guberlab/
├── index.html               # Dashboard web completo (single file)
├── guberlab_firmware.ino    # Firmware ESP32
└── README.md                # Esta documentação
```

---

## Licença

Projeto de código aberto. Sinta-se livre para usar, modificar e distribuir.  
Se usar em algum projeto, uma estrela no repositório é sempre bem-vinda! ⭐

Entre em contato: Gubernavitsuporte@gmail.com
