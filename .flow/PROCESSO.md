# Processo de engenharia — ReflowOven

Contrato entre as sessões de agentes (Gerente, Dev, Q.A.) e o dono do projeto.
Nenhum agente improvisa fora do que está aqui.

**Versão 2 — GitHub nativo.** A v1 fazia o código circular como patch dentro de
documento porque os containers dos agentes não conseguiam escrever no GitHub. Uma
sessão rodando na máquina do Gustavo consegue: `git push` autentica pelo
Credential Manager e o `gh` fala com a API. Toda a maquinaria de contorno —
`flow/MAIN.patch`, `base_mainpatch`, patch colado em bloco ```` ```diff ```` —
sai. O que era protocolo manual passa a ser mecanismo.

O que a v1 acertou e continua valendo: papéis separados, um escritor por artefato,
estado explícito, e a Definition of Done da seção 6.

---

## 1. Onde o estado vive

Tudo no GitHub, em `GustavoCCosta/ReflowOven`. Não existe mais armazenamento
paralelo.

| Coisa | Onde | Quem escreve |
| --- | --- | --- |
| Código aprovado | branch `main` | só merge de PR |
| Trabalho em curso | branch `dev/<ID>` | Dev |
| Backlog | issues | Gerente cria; Dev muda label de estado |
| Pull request | PR de verdade, com `Closes #N` | Dev |
| Revisão | review nativo (Approve / Request changes) | Q.A. |
| Aprovação e merge | botão de merge | Gerente |
| Quadro | GitHub Projects, ou a lista de issues por label | Gerente |
| Gate objetivo | `.github/workflows/ci.yml` | ninguém — ver seção 9 |

**Não há mais `flow/MAIN.patch`.** O trabalho aprovado é a própria `main`. A
pergunta "estamos olhando o mesmo código?" era respondida por um sha256 conferido
à mão; agora é `git rev-parse HEAD`.

A pasta `C:\zephyrproject\flow\` e os docs do Project que a originaram ficam como
**registro histórico**. Nada os lê a partir daqui.

## 2. Como o código circula

```
origin/main
    │  git switch -c dev/<ID> origin/main
    ▼
branch de trabalho  ── push ──►  PR  ── CI + review ──►  merge  ──►  origin/main
```

- Um branch por issue, nomeado `dev/<ID>` (ex.: `dev/RFO-B05`).
- Branch sempre criado a partir de `origin/main` atualizado. Se a `main` andar
  durante o trabalho, rebase — não merge, para o histórico do PR continuar legível.
- O PR é aberto contra `main`. O CI roda sozinho.
- Ninguém faz push direto na `main`. A única exceção está na seção 9.

### As duas regras que evitam corrida

1. **Um único escritor por artefato.** O Dev é o único que faz push no branch
   `dev/<ID>`. O Q.A. escreve em reviews e comentários, nunca no branch do Dev.
   O Gerente escreve em issues e no botão de merge.

2. **O estado está em label**, não em nome de arquivo. Descobrir o que fazer é
   `gh issue list --label estado:pronto`.

## 3. Máquina de estados da issue

```
triagem ──► pronto ──► fazendo ──► revisao ──► merge (issue fecha)
[Gerente]  [Gerente]    [Dev]       [Dev]        [Gerente]
                          ▲            │
                          └── ajustes ◄┘
                                [Q.A.]
```

Estado = exatamente uma label `estado:*`. Sem label `estado:*` significa
`triagem`. `aprovado` e `concluido` não são labels: o merge fecha a issue via
`Closes #N`, e a data do merge é o registro.

| Transição | Quem pode | O que acontece |
| --- | --- | --- |
| `→ triagem` | qualquer um | abre issue sem label `estado:*` |
| `triagem → pronto` | **só Gerente** | escreve o critério de aceite e põe `estado:pronto` |
| `pronto → fazendo` | **só Dev** | troca para `estado:fazendo`, se atribui, cria o branch |
| `fazendo → revisao` | **só Dev** | abre o PR, troca para `estado:revisao` |
| `revisao → ajustes` | **só Q.A.** | *Request changes* no PR, troca para `estado:ajustes` |
| `ajustes → revisao` | **só Dev** | responde cada ponto, faz push, pede nova revisão |
| `revisao → merge` | **só Gerente** | confere, aprova e faz merge |

Um agente que precise de uma transição que não é dele **pede por escrito num
comentário da issue**; não executa.

## 4. Ordem de puxada do backlog

O Dev puxa o primeiro que couber:

1. `estado:ajustes` — retrabalho vem antes de trabalho novo
2. `estado:pronto` + `gate` + `prio:critico`
3. `estado:pronto` + `gate`
4. `estado:pronto` por prioridade: `critico` → `alto` → `medio` → `baixo`

Empate dentro de um bucket: vence o ID menor; entre `RFO-G*` e `RFO-B*` de mesmo
bucket, o `G` vem antes.

Nunca puxa issue com `precisa-hardware` ou `precisa-humano`.

## 5. Uma issue por PR — contrato do PR

**Um PR fecha exatamente uma issue.** O corpo traz `Closes #N` uma única vez. O
job `processo` do CI reprova o PR se encontrar zero ou mais de uma — a regra é
mecânica, não confiança.

Por que uma só: PR que resolve dois tickets não pode ser revertido sem desfazer
os dois, e a revisão perde o foco. Q.A. que receber PR com dois `Closes` devolve
pedindo divisão.

O PR usa o template de `.github/pull_request_template.md`, e as seções são
obrigatórias:

- **`## O que muda`** — o escopo, em prosa curta.
- **`## Como isso foi provado`** — a saída do teste falhando sem o patch e
  passando com ele. Não "os testes passam": a saída.
- **`## Análise de segurança`** — obrigatória se toca `heater.c`, `temp.c` ou
  `controller.c`. A pergunta a responder não é "isso funciona?", é **"como isso
  deixa a resistência ligada?"**.
- **`## Fora de escopo`** — o que você viu e deliberadamente não mexeu.

## 6. Definition of Done

O Q.A. só aprova, e o Gerente só faz merge, se **tudo** abaixo for verdade. Os
três primeiros itens o CI mede; os outros três são humanos e o CI não os enxerga.

| # | Item | Quem verifica |
| --- | --- | --- |
| 1 | `west twister -T tests -p native_sim` passa | CI, job `ztest` |
| 2 | Matriz de modularidade compila — cada `CONFIG_REFLOW_*` liga e desliga | CI, job `modularidade` |
| 3 | `-Wall -Wextra -Werror` sem warning novo | CI, job `logica` |
| 4 | **Existe um teste que falha sem o patch e passa com o patch**, verificado com `flow mutate` — não presumido | Q.A. |
| 5 | O critério de aceite da issue foi atendido, item por item | Q.A. |
| 6 | Se toca caminho de segurança, a revisão diz **qual cenário de falha foi tentado e o que foi observado** | Q.A. |

O item 4 é o único que não depende de ninguém acreditar em ninguém, e é o único
que o CI não substitui. Ele fica.

> **Orientação permanente sobre o item 4.** Para PR que **cria arquivo novo**, a
> mutação passa a falhar na *compilação*, não na asserção — os testes deixam de
> compilar porque o `#include` aponta para o arquivo apagado. Satisfaz a letra,
> informa menos: um teste vazio que só incluísse o header novo "falharia" igual.
> Nesses casos a versão forte é a **mutação comportamental à mão** — trocar o
> corpo da função nova pelo comportamento de antes do patch, para obter asserções
> vermelhas.

## 7. Papéis, e quem pode o quê

| | Gerente | Dev | Q.A. |
| --- | --- | --- | --- |
| Cria e prioriza issue | ✅ | ❌ | ❌ |
| Escreve código de produção | ❌ | ✅ | ❌ |
| Escreve teste | ❌ | ✅ | ✅ |
| Emite veredito no PR | ✅ | ❌ | ✅ |
| Faz merge | ✅ | ❌ | ❌ |
| Altera o CI ou este arquivo | ❌ | ❌ | ❌ |

### A independência que este arranjo perde, e o que a substitui

Na v1 o Q.A. aprovava e o Gerente consolidava — dois papéis, duas sessões, e o
GERENTE.md dizia explicitamente *"nunca aprova PR no lugar do Q.A."*. Se o
Gerente acumular aprovação e merge, essa checagem dupla desaparece, e é honesto
dizer em voz alta em vez de fingir que o organograma continua igual.

Duas coisas a compensam, e só valem se forem respeitadas:

1. **O CI agora é gate objetivo por PR**, o que na v1 não existia — a seção 8 da
   v1 admitia que não havia CI por PR e que o gate era o Q.A. rodando twister na
   mão. Os itens 1 a 3 do DoD deixaram de depender de julgamento.
2. **Quem aprova não escreve o código.** Esta é a linha que não se cruza. Um
   Gerente que aprova e faz merge está proibido de escrever produção — senão o
   autor vira o próprio revisor e não sobra checagem nenhuma.

Enquanto não houver sessão de Q.A. independente ativa, o Gerente registra no PR
que a revisão foi feita sem segunda opinião. Item de segurança (`gate`) aprovado
sem Q.A. independente é reportado ao Gustavo, não silenciado.

### O veredito de Q.A. é uma review nativa, não um comentário

Decidido em 2026-09-04 (RFO-G13, #57): o Q.A. tem **identidade própria no
GitHub**, a conta `QualityAssurance2007`, colaboradora com `push` e `triage` e
autenticada com PAT *classic* de escopo `repo` — **sem** `workflow`, porque a
seção 9 reserva a régua ao humano. Foi a primeira das duas saídas da issue; a
outra (aceitar o contrato no lugar do mecanismo) fica descartada.

Consequências que valem como regra:

- O veredito do Q.A. sai por `gh pr review --approve` / `--request-changes` e é
  achável em `gh pr view <N> --json reviews`. **Veredito em comentário comum não
  conta** e não satisfaz a proteção da `main`.
- A identidade vai por `GH_TOKEN` **por chamada**:
  `GH_TOKEN=$(gh auth token --user QualityAssurance2007) gh pr review ...`.
  **Ninguém roda `gh auth switch`** — a conta ativa do `gh` é estado global da
  máquina e trocá-la derruba a sessão que estiver ao lado (RFO-G18, #74).
- "Ninguém aprova o próprio PR" (seção 8) volta a ser mecanismo: o GitHub recusa
  review na própria PR, e quem aprova não é a conta que abriu.

## 8. Limites duros

- Ninguém altera este arquivo nem `.github/workflows/` — ver seção 9.
- Ninguém enfraquece, pula ou remove teste para a suíte passar. Teste que falha é
  bug encontrado, não obstáculo.
- Ninguém conclui issue `precisa-hardware`.
- Ninguém faz push direto na `main`.
- Ninguém faz merge com CI vermelho.
- Ninguém aprova o próprio PR.
- Ao esgotar o backlog, o agente **para e relata**; não inventa trabalho, não
  refatora por conta própria, não "melhora" código que ninguém pediu.
- Segredo não entra em arquivo versionado. Token vai em variável de ambiente ou
  no Credential Manager, nunca no repositório.

## 9. O gate de CI — quem está sendo medido não mexe na régua

`.github/workflows/ci.yml` é o gate objetivo. O Q.A. pode reprovar um PR com CI
verde; ninguém aprova nem faz merge com CI vermelho.

**Nenhum agente altera `.github/workflows/` nem este arquivo.** O job `processo`
do CI reprova automaticamente qualquer PR que toque um dos dois. Mudança nesses
arquivos exige a label `precisa-humano`, autorização explícita do Gustavo, e é
ele quem decide como ela entra.

A única exceção é o **bootstrap do processo**, e ela é de uso único: a v1 vivia
em documento e a v2 precisa existir na `main` antes de qualquer PR poder ser
julgado por ela. Esse commit vai direto na `main`, com autorização humana
registrada na mensagem. Depois disso a regra acima vale sem exceção.

### Proteção de branch

O gate só é gate se o GitHub o impuser. Configuração esperada em `main`
(**é do Gustavo — agente nenhum tem permissão de mudar isto**):

- Pull request obrigatório antes do merge, com pelo menos 1 aprovação.
- Required status checks: `Logica pura + host_sim`, `ztest (native_sim)`,
  `Matriz de modularidade (native_sim)`, `Contrato de PR`.
- Bloquear push direto e force-push na `main`.

Estado real em 2026-09-04, para ninguém supor garantia que não existe:

- **Ligado:** PR obrigatório, `required_approving_review_count: 1` (era `0` — a
  aprovação era impossível antes do RFO-G13, #57), `dismiss_stale_reviews: true`
  (push novo derruba a aprovação e o Q.A. reaprova), sem force-push, sem
  deleção, histórico linear, resolução de conversa obrigatória.
- **Não ligado:** os quatro *required status checks* acima. Merge com CI
  vermelho ainda é contrato, não mecanismo — e continua proibido pela seção 8.
- `enforce_admins: false`: o Gustavo passa por cima quando precisa. Agente não.

Enquanto a proteção não estiver ligada, as garantias são de contrato e não de
mecanismo — exatamente a fraqueza que a v2 existe para remover.

## 10. Ambiente

A aplicação mora dentro do workspace west, mesmo layout do PC do Gustavo:

```
C:\zephyrproject\          <- workspace west  (ZEPHYR_WS)
  ├── zephyr\
  ├── .venv\               <- Scripts\ no Windows, bin\ no Linux
  └── reflow_oven\         <- esta aplicação  (REFLOW_DIR)
```

O `.flow/bin/flow` continua útil para `test`, `matrix` e `mutate`. Os
subcomandos `base`, `work`, `patch`, `apply` e `mainpatch` pertenciam ao fluxo de
patch-em-documento e estão obsoletos.

**`native_sim` não roda no Windows** — o twister filtra com *"Native platform
requires Linux"*. Consequências:

- `flow test` local no Windows: use `--board qemu_x86` com
  `--timeout-multiplier 6`. Mede a mesma suíte.
- `flow matrix` local no Windows: **não roda**, porque só existe
  `boards/native_sim.overlay`. Quem mede o item 2 do DoD é o CI.
- Para rodar `native_sim` localmente é preciso um Linux (WSL serve) com o
  workspace montado por `.flow/bin/setup_zephyr.sh`.
