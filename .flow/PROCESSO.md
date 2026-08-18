# Processo de engenharia — ReflowOven

Contrato entre as três sessões (Gerente, Dev, Q.A.). Nenhum agente improvisa
fora do que está aqui.

---

## 1. Onde o estado vive

As três sessões rodam em containers isolados. Elas **não** compartilham disco, e
este ambiente **não permite escrita no GitHub** (o proxy libera leitura e barra
push e API). Logo:

| Coisa | Onde | Quem escreve |
| --- | --- | --- |
| Código base | GitHub, `origin/main` — **somente leitura** | ninguém (só o Gustavo, do PC dele) |
| Catálogo do backlog | doc `flow/BACKLOG.md` | Gerente |
| Ticket vivo | doc `flow/backlog/<estado>--<ID>.md` | quem detém o estado |
| Pull request | doc `flow/prs/PR-<n>--<ID>.md` | Dev |
| Revisão | doc `flow/reviews/PR-<n>--<veredito>.md` | Q.A. |
| Trabalho aprovado | doc `flow/MAIN.patch` | Gerente |
| Quadro para o Gustavo | doc `flow/QUADRO.md` | Gerente |
| Diário de bordo | doc `flow/log-{dev,qa,gerente}.md` | cada um o seu |

"doc" = documento do Project do Claude, lido com `project_read` e escrito com
`project_write`. É o único armazenamento que as três sessões enxergam.

### As duas regras que evitam corrida

1. **Um único escritor por documento.** Nunca dois agentes escrevem o mesmo doc.
   Onde o dono muda com o estado (tickets), a exclusividade vem da tabela de
   transições da seção 3 — em cada estado só um papel pode escrever.

2. **O estado está no nome do arquivo.** Descobrir o que fazer é listar os docs
   com `project_info`, sem abrir nenhum. Mudar de estado é
   `project_write` no nome novo seguido de `project_delete` no antigo — **nessa
   ordem**, para que uma falha no meio deixe uma duplicata visível em vez de um
   ticket desaparecido.

Se você encontrar o mesmo ID em dois estados, é uma transição interrompida:
apague o **anterior** (o da esquerda na sequência da seção 3) e registre no seu log.

---

## 2. Como o código circula sem push

Ninguém consegue escrever no GitHub a partir daqui. O código anda assim:

```
origin/main (GitHub, leitura)
      │  git clone
      ▼
  cópia local da sessão
      │  git apply flow/MAIN.patch      <- trabalho já aprovado
      ▼
   BASE DE TRABALHO   (o que todos consideram "o estado atual do projeto")
      │  Dev edita e gera um diff
      ▼
  doc flow/prs/PR-<n>--<ID>.md          <- o patch vive dentro do documento
```

- **Dev** entrega um diff contra a BASE, não contra `origin/main`.
- **Q.A.** monta a BASE, aplica o patch do PR e testa.
- **Gerente**, ao aprovar, regenera `flow/MAIN.patch`: monta a BASE, aplica o
  patch do PR, e grava `git diff origin/main` inteiro. `MAIN.patch` é sempre um
  diff completo contra `origin/main`, **nunca** um empilhamento de patches.
- **Gustavo** aplica `MAIN.patch` no PC dele e dá push quando quiser.

Todo patch traz no cabeçalho o commit de `origin/main` sobre o qual foi gerado.
Se o Gustavo empurrar algo novo, `MAIN.patch` precisa ser regenerado sobre a
`main` nova — trabalho do Gerente, e ele avisa no `QUADRO.md`.

---

## 3. Máquina de estados do ticket

```
triagem ──► pronto ──► fazendo ──► revisao ──► aprovado ──► concluido
[Gerente]  [Gerente]    [Dev]        [Dev]       [Q.A.]     [Gerente]
                          ▲            │
                          └── ajustes ◄┘
                              [Q.A.]
```

| Transição | Quem pode | O que acontece nos docs |
| --- | --- | --- |
| `→ triagem` | qualquer um | linha nova em `flow/BACKLOG.md` (peça ao Gerente) |
| `triagem → pronto` | **só Gerente** | cria `flow/backlog/pronto--<ID>.md` |
| `pronto → fazendo` | **só Dev** | renomeia para `fazendo--<ID>.md` |
| `fazendo → revisao` | **só Dev** | cria `flow/prs/PR-<n>--<ID>.md`, renomeia ticket |
| `revisao → aprovado` | **só Q.A.** | cria `flow/reviews/PR-<n>--aprovado.md` |
| `revisao → ajustes` | **só Q.A.** | cria `flow/reviews/PR-<n>--ajustes.md` |
| `ajustes → fazendo` | **só Dev** | renomeia ticket |
| `aprovado → concluido` | **só Gerente** | regenera `MAIN.patch`, apaga ticket e PR |

Um agente que precisar de uma transição que não é dele **pede por escrito no seu
próprio log**; não executa. É isso que impede o Dev de aprovar o próprio trabalho.

---

## 4. Ordem de puxada do backlog

O Dev puxa o primeiro que couber:

1. `ajustes--*` (retrabalho vem antes de trabalho novo)
2. `pronto--*` com `gate: true` e `prio: critico`
3. `pronto--*` com `gate: true`
4. `pronto--*` por prioridade: critico → alto → medio → baixo

Nunca puxa ticket marcado `precisa-hardware` ou `precisa-humano`.

---

## 5. Formato dos documentos

**Ticket** (`flow/backlog/<estado>--<ID>.md`), cabeçalho obrigatório:

```yaml
---
id: RFO-B05
estado: pronto
prio: critico
area: seguranca
gate: true
bloqueio:            # vazio, precisa-hardware ou precisa-humano
pr:                  # número do PR quando houver
---
```

Depois do cabeçalho: descrição do defeito, arquivo:linha, cenário de falha e uma
seção **`## Critério de aceite`** com itens verificáveis. Ticket sem critério de
aceite não sai de `triagem`.

**PR** (`flow/prs/PR-<n>--<ID>.md`):

```yaml
---
pr: 7
id: RFO-B05
base_origin: 58ca3e6        # commit de origin/main
base_mainpatch: sha256:...  # hash do MAIN.patch usado
---
```

Seções obrigatórias: `## O que muda`, `## Como isso foi provado` (saída do teste
falhando sem o patch e passando com ele), `## Análise de segurança` se tocar
`heater.c`/`temp.c`/`controller.c`, `## Fora de escopo`, e por fim o diff dentro
de um bloco ` ```diff `.

**Revisão** (`flow/reviews/PR-<n>--<veredito>.md`): veredito, cenários testados
com evidência, e — se reprovado — o que falha, com quais entradas, e como reproduzir.

---

## 6. Definition of Done

O Q.A. só aprova se **tudo** abaixo for verdade:

1. `west twister -T tests -p native_sim` passa.
2. `flow matrix` passa (todas as combinações de `CONFIG_REFLOW_*` compilam).
   *Modularidade é o requisito central do projeto; quebrar a build com uma
   feature desligada é reprovação automática.*
3. **Existe um teste que falha sem o patch e passa com o patch**, verificado com
   `flow mutate` — não presumido.
4. O critério de aceite do ticket foi atendido, item por item.
5. Sem warning novo em `-Wall -Wextra`.
6. Se toca caminho de segurança (aquecedor, corte de sobretemperatura, fail-safe,
   leitura de termopar), a revisão diz **qual cenário de falha foi tentado e o
   que foi observado**. Aprovação sem esse parágrafo não conta.

---

## 7. Limites duros

- Ninguém altera este arquivo nem `.github/workflows/` sem `precisa-humano`.
- Ninguém enfraquece, pula ou remove teste para a suíte passar. Teste que falha é
  bug encontrado, não obstáculo.
- Ninguém conclui ticket `precisa-hardware`.
- Ninguém escreve em doc de outro papel.
- Ao esgotar o backlog, o agente **para e relata**; não inventa trabalho.
- Nenhum agente tenta `git push` — vai falhar, e insistir só queima turno.

---

## 8. O que este arranjo não dá

Sendo honesto sobre o custo de não ter o GitHub:

- **Não há CI automático por PR.** O Actions só roda quando o Gustavo empurra.
  O gate real passa a ser o Q.A. rodando `twister` e `flow matrix` na mão.
- **Não há proteção de branch.** As garantias são de contrato, não de mecanismo:
  dependem de os agentes seguirem este documento.
- Por isso o passo 3 da seção 6 (`flow mutate`) fica ainda mais importante — é a
  única verificação que não depende de ninguém acreditar em ninguém.
