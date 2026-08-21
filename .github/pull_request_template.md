<!--
Contrato do PR: PROCESSO.md secao 5. Um PR fecha UMA issue.
O job `processo` do CI reprova o PR se `Closes #N` aparecer zero ou duas vezes.
-->

Closes #

## O que muda

<!-- O escopo, em prosa curta. Escopo extra e motivo de devolucao. -->

## Como isso foi provado

<!--
A saida do teste, nao a afirmacao de que ele passa.

  1. o teste FALHANDO sem o patch  (flow mutate, ou mutacao comportamental a mao)
  2. o teste PASSANDO com o patch

Cole as duas. Sem isso o Q.A. devolve — item 4 da Definition of Done.
-->

```
```

## Análise de segurança

<!--
OBRIGATORIA se este PR toca src/core/heater.c, src/core/temp.c ou
src/core/controller.c. Se nao toca, escreva "nao se aplica" e apague o resto.

A pergunta nao e "isso funciona?" — e "COMO ISSO DEIXA A RESISTENCIA LIGADA?".

Responda o que for pertinente:
  - se esta thread morrer agora, o SSR desliga?
  - se a leitura do termopar travar, o corte de 270 C ainda dispara?
  - o que acontece entre o reset e a inicializacao do driver de GPIO?
  - valor NaN/Inf do sensor contamina o integrador?
  - segunda execucao, limites, estouro, ordem de inicializacao?
-->

## Fora de escopo

<!-- O que voce viu e deliberadamente nao mexeu. Se nao ha nada, escreva "nada". -->

---

### Definition of Done (PROCESSO.md secao 6)

Itens 1 a 3 sao medidos pelo CI — nao marque à mão, deixe o CI falar.

- [ ] **4.** Existe teste que **falha sem o patch** e passa com ele, *verificado* com `flow mutate` (saída colada acima)
- [ ] **5.** O critério de aceite da issue foi atendido, item por item
- [ ] **6.** Se toca caminho de segurança: a análise acima diz **qual cenário de falha foi tentado e o que foi observado**
