/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the two pieces of logic that decide whether a board gets
 * soldered or cooked: the PID and the profile state machine.
 * Run with:  west twister -T tests -p native_sim
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "pid.h"
#include "profile.h"
#include "net/cmdparse.h"
#include "net/httpgate.h"
#include "net/profiles_json.h"
#include "net/sendbudget.h"
#include "tempguard.h"

/* ------------------------------------------------------------------ PID */

static const struct pid_cfg cfg = {
	.kp = 40.0f,
	.ki = 0.5f,
	.kd = 200.0f,
	.out_min = 0.0f,
	.out_max = 1000.0f,
	.i_min = -500.0f,
	.i_max = 500.0f,
	.d_alpha = 0.2f,
};

ZTEST(reflow_pid, test_proportional_only)
{
	struct pid_cfg p = cfg;
	struct pid_state st;
	float out;

	p.ki = 0.0f;
	p.kd = 0.0f;
	pid_reset(&st);

	out = pid_step(&p, &st, 100.0f, 90.0f, 0.25f, NULL);
	zassert_within(out, 400.0f, 0.01f, "expected kp*err = 400, got %f",
		       (double)out);
}

ZTEST(reflow_pid, test_output_is_clamped)
{
	struct pid_state st;
	float hot, cold;

	pid_reset(&st);
	cold = pid_step(&cfg, &st, 250.0f, 25.0f, 0.25f, NULL);
	zassert_equal(cold, 1000.0f, "large positive error must saturate high");

	pid_reset(&st);
	hot = pid_step(&cfg, &st, 25.0f, 250.0f, 0.25f, NULL);
	zassert_equal(hot, 0.0f, "negative error must saturate low");
}

ZTEST(reflow_pid, test_integral_does_not_wind_up)
{
	struct pid_state st;
	struct pid_terms terms;

	pid_reset(&st);

	/* 200 s of a huge error: the output is pinned at max the whole time. */
	for (int i = 0; i < 800; i++) {
		(void)pid_step(&cfg, &st, 250.0f, 25.0f, 0.25f, &terms);
	}
	zassert_true(terms.i <= cfg.i_max,
		     "integral %f exceeded the clamp", (double)terms.i);

	/*
	 * Now the measurement overshoots. With anti-windup the controller must
	 * back off within a couple of steps, not stay stuck at full power.
	 */
	for (int i = 0; i < 4; i++) {
		(void)pid_step(&cfg, &st, 250.0f, 265.0f, 0.25f, &terms);
	}
	zassert_true(terms.out < 1000.0f,
		     "controller stayed saturated after overshoot (out=%f)",
		     (double)terms.out);
}

ZTEST(reflow_pid, test_no_derivative_kick_on_setpoint_step)
{
	struct pid_cfg p = cfg;
	struct pid_state st;
	float a, b;

	p.ki = 0.0f;
	pid_reset(&st);

	/* Steady measurement, setpoint jumps: D must contribute nothing. */
	(void)pid_step(&p, &st, 30.0f, 30.0f, 0.25f, NULL);
	a = pid_step(&p, &st, 30.0f, 30.0f, 0.25f, NULL);
	b = pid_step(&p, &st, 230.0f, 30.0f, 0.25f, NULL);

	zassert_within(a, 0.0f, 0.01f, "no error, no output");
	zassert_equal(b, 1000.0f, "step should go to full power, got %f",
		      (double)b);
}

ZTEST_SUITE(reflow_pid, NULL, NULL, NULL, NULL, NULL);

/* -------------------------------------------------------------- profile */

static const struct reflow_profile test_prof = {
	.name = "test",
	.n_stages = 3,
	.stages = {
		{ "ramp", 100000, 10000, REFLOW_STAGE_RAMP },
		{ "hold", 100000, 10000, REFLOW_STAGE_SOAK },
		{ "cool",  50000, 20000, REFLOW_STAGE_COOL },
	},
	.abort_mc = 150000,
	.tol_mc = 2000,
	.grace_ms = 5000,
};

ZTEST(reflow_profile, test_builtin_table_is_sane)
{
	zassert_true(reflow_profile_count() > 0, "no built-in profiles");
	zassert_is_null(reflow_profile_get(reflow_profile_count()),
			"out of range index must return NULL");

	for (uint8_t i = 0; i < reflow_profile_count(); i++) {
		const struct reflow_profile *p = reflow_profile_get(i);

		zassert_true(p->n_stages > 0 && p->n_stages <= REFLOW_MAX_STAGES,
			     "profile %u has %u stages", i, p->n_stages);
		zassert_equal(p->stages[p->n_stages - 1].kind, REFLOW_STAGE_COOL,
			      "profile %u must end on a cooling stage", i);
		for (uint8_t s = 0; s < p->n_stages; s++) {
			zassert_true(p->stages[s].target_mc < p->abort_mc,
				     "profile %u stage %u targets at or above the abort level",
				     i, s);
		}
	}
}

ZTEST(reflow_profile, test_ramp_setpoint_is_interpolated)
{
	struct reflow_run run;

	reflow_run_start(&run, &test_prof, 20000);
	zassert_equal(run.setpoint_mc, 20000, "ramp must start at the entry temp");

	/* Half the ramp: setpoint halfway between 20 and 100 degC. */
	(void)reflow_run_tick(&run, &test_prof, 5000, 20000, 20000);
	zassert_within(run.setpoint_mc, 60000, 500,
		       "expected ~60000 mC, got %d", run.setpoint_mc);

	/* Setpoint never overshoots the stage target. */
	(void)reflow_run_tick(&run, &test_prof, 4000, 20000, 20000);
	zassert_true(run.setpoint_mc <= 100000, "setpoint %d above target",
		     run.setpoint_mc);
}

ZTEST(reflow_profile, test_stage_advances_only_when_hot_enough)
{
	struct reflow_run run;

	reflow_run_start(&run, &test_prof, 20000);

	/* Time is up but the oven is cold: stay in stage 0. */
	(void)reflow_run_tick(&run, &test_prof, 10000, 20000, 20000);
	zassert_equal(run.stage, 0, "advanced with the oven still cold");

	/* Target reached: move on, and the new stage starts from here. */
	(void)reflow_run_tick(&run, &test_prof, 100, 99000, 99000);
	zassert_equal(run.stage, 1, "should have entered the soak");
	zassert_equal(run.stage_ms, 0, "stage timer must restart");
	zassert_equal(run.stage_start_mc, 99000, "entry temperature not recorded");
}

ZTEST(reflow_profile, test_grace_period_expiry_is_a_fault)
{
	struct reflow_run run;
	enum reflow_run_result res = REFLOW_RUN_ACTIVE;

	reflow_run_start(&run, &test_prof, 20000);

	/* Oven never heats: nominal 10 s + 5 s grace, then fault. */
	for (int i = 0; i < 200 && res == REFLOW_RUN_ACTIVE; i++) {
		res = reflow_run_tick(&run, &test_prof, 250, 21000, 21000);
	}
	zassert_equal(res, REFLOW_RUN_ERR_TIMEOUT, "expected a timeout fault");
	zassert_equal(reflow_run_tick(&run, &test_prof, 250, 21000, 21000),
		      REFLOW_RUN_ERR_TIMEOUT, "fault must latch");
}

ZTEST(reflow_profile, test_overtemp_aborts)
{
	struct reflow_run run;

	reflow_run_start(&run, &test_prof, 20000);
	zassert_equal(reflow_run_tick(&run, &test_prof, 100, 151000, 151000),
		      REFLOW_RUN_ERR_OVERTEMP, "abort level not enforced");
}

ZTEST(reflow_profile, test_heater_is_off_while_cooling)
{
	struct reflow_run run;

	reflow_run_start(&run, &test_prof, 20000);
	zassert_true(reflow_run_heater_allowed(&run, &test_prof),
		     "heater should be allowed on the ramp");

	run.stage = 2; /* cooling stage */
	zassert_false(reflow_run_heater_allowed(&run, &test_prof),
		      "heater must be forced off while cooling");
}

ZTEST(reflow_profile, test_full_run_completes)
{
	struct reflow_run run;
	int32_t temp = 20000;
	enum reflow_run_result res = REFLOW_RUN_ACTIVE;

	reflow_run_start(&run, &test_prof, temp);

	for (int i = 0; i < 2000 && res == REFLOW_RUN_ACTIVE; i++) {
		/* Crude oven: follows the setpoint, cools when the heater is off. */
		if (reflow_run_heater_allowed(&run, &test_prof)) {
			temp += (run.setpoint_mc - temp) / 5;
		} else {
			temp -= 1500;
		}
		res = reflow_run_tick(&run, &test_prof, 250, temp, temp);
	}

	zassert_equal(res, REFLOW_RUN_DONE, "run did not finish (res=%d, stage=%u)",
		      res, run.stage);
	zassert_true(temp <= 50000, "finished above the cool-down target: %d", temp);
}

/*
 * RFO-B04, com o modelo de resfriamento que o RFO-B32 (#30) pediu.
 *
 * test_full_run_completes acima esfria a 1500 mC por tique de 250 ms -- 6 degC/s
 * constantes, taxa que nenhum forno passivo tem -- e por isso nunca viu este
 * defeito. Aqui o resfriamento e Newtoniano: cada tique perde uma fracao de
 * (T - ambiente) dada pela constante de tempo termica do forno. Com tau = 300 s,
 * dentro da faixa de 200 a 600 s de uma torradeira com a porta fechada, cair de
 * 245 degC para 50 degC leva ~650 s. O orcamento antigo do estagio 'cool' era
 * nominal 180 s + grace 60 s = 240 s, e a corrida terminava em FAULT_TIMEOUT
 * depois de soldar a placa corretamente.
 *
 * Usa o perfil embutido de verdade, nao test_prof: o defeito esta nos numeros
 * dos perfis que o firmware entrega.
 */
#define OVEN_TAU_MS 300000U
#define AMBIENT_MC  25000

ZTEST(reflow_profile, test_resfriamento_passivo_realista_nao_e_falta)
{
	const struct reflow_profile *p = reflow_profile_get(0);
	const uint32_t dt_ms = 250;
	struct reflow_run run;
	int32_t temp = AMBIENT_MC;
	enum reflow_run_result res = REFLOW_RUN_ACTIVE;
	uint32_t elapsed_ms = 0;
	uint32_t limit_ms;

	zassert_not_null(p, "perfil 0 nao existe");
	zassert_equal(p->stages[p->n_stages - 1].kind, REFLOW_STAGE_COOL,
		      "este teste assume que o perfil termina esfriando");

	limit_ms = reflow_profile_max_ms(p);
	reflow_run_start(&run, p, temp);

	while (res == REFLOW_RUN_ACTIVE && elapsed_ms <= limit_ms) {
		if (reflow_run_heater_allowed(&run, p)) {
			/* Forno aquecido: segue o setpoint com atraso curto. */
			temp += (run.setpoint_mc - temp) / 8;
		} else {
			/* Resfriamento passivo, primeira ordem, sem aquecedor. */
			temp -= (int32_t)(((int64_t)(temp - AMBIENT_MC) * dt_ms) /
					  OVEN_TAU_MS);
		}
		res = reflow_run_tick(&run, p, dt_ms, temp, temp);
		elapsed_ms += dt_ms;
	}

	/*
	 * A assercao que fica vermelha sem o patch: REFLOW_RUN_ERR_TIMEOUT no
	 * ultimo estagio, com a placa ja soldada e o aquecedor desligado desde
	 * o inicio do resfriamento.
	 */
	zassert_equal(res, REFLOW_RUN_DONE,
		      "corrida terminou em %d no estagio %u (%s) apos %u ms: "
		      "resfriamento passivo com tau=%u ms nao e falta",
		      res, run.stage, p->stages[run.stage].name, elapsed_ms,
		      OVEN_TAU_MS);
	zassert_true(temp <= p->stages[p->n_stages - 1].target_mc,
		     "terminou acima do alvo de resfriamento: %d mC", temp);
	zassert_true(temp > AMBIENT_MC - 1000,
		     "o modelo esfriou abaixo do ambiente: %d mC", temp);
}

/*
 * O outro lado da mesma moeda: o orcamento maior e do estagio de resfriamento e
 * de mais nenhum. Um estagio de aquecimento que estoura continua sendo falta, e
 * tem de ser -- ali o elemento esta ligado e a placa esta sendo cozida a uma
 * temperatura desconhecida. Sem esta, "consertar" o RFO-B04 aumentando
 * grace_ms passaria.
 */
ZTEST(reflow_profile, test_estagio_de_aquecimento_continua_com_a_graca_curta)
{
	const struct reflow_profile *p = reflow_profile_get(0);
	const struct reflow_stage *st = &p->stages[0];
	const uint32_t dt_ms = 250;
	struct reflow_run run;
	enum reflow_run_result res = REFLOW_RUN_ACTIVE;
	uint32_t elapsed_ms = 0;

	zassert_true(st->kind != REFLOW_STAGE_COOL, "estagio 0 deveria aquecer");

	reflow_run_start(&run, p, AMBIENT_MC);

	/* Forno morto: nunca sai do ambiente. */
	while (res == REFLOW_RUN_ACTIVE && elapsed_ms <= REFLOW_COOL_GRACE_MS) {
		res = reflow_run_tick(&run, p, dt_ms, AMBIENT_MC, AMBIENT_MC);
		elapsed_ms += dt_ms;
	}

	zassert_equal(res, REFLOW_RUN_ERR_TIMEOUT,
		      "estagio de aquecimento parado nao virou falta (res=%d)", res);
	zassert_true(elapsed_ms <= st->nominal_ms + p->grace_ms + dt_ms,
		      "falta saiu em %u ms, depois do nominal %u + graca %u: o "
		      "orcamento do resfriamento vazou para o aquecimento",
		      elapsed_ms, st->nominal_ms, p->grace_ms);
}

/*
 * RFO-B34, cenario C da issue: um degrau real e persistente acima do abort do
 * perfil e abaixo do corte absoluto de 270 degC.
 *
 * O valor filtrado vem do reflow_spike_filter() DE VERDADE, nao de um numero
 * escolhido a mao: um teste que encenasse a supressao passaria mesmo que o
 * filtro nao suprimisse nada, e nao teria exercitado o defeito. Antes deste
 * patch nenhuma das duas barreiras via a subida - a do perfil porque le o valor
 * suprimido, a absoluta porque 265 degC ainda esta abaixo dela.
 */
ZTEST(reflow_profile, test_abort_do_perfil_enxerga_a_amostra_crua)
{
	const int32_t raw_mc = 151000;   /* acima do abort de 150 degC do test_prof */
	struct reflow_spike spike;
	struct reflow_run run;
	int32_t filtered;
	enum reflow_spike_result sres;

	reflow_spike_reset(&spike);
	(void)reflow_spike_filter(&spike, 25000, &filtered);
	zassert_equal(filtered, 25000, "primeira amostra deveria passar intacta");

	reflow_run_start(&run, &test_prof, filtered);

	/* O degrau e maior que REFLOW_SPIKE_MAX_STEP_MC, entao o filtro segura. */
	sres = reflow_spike_filter(&spike, raw_mc, &filtered);
	zassert_equal(sres, REFLOW_SPIKE_REJECT,
		      "o filtro nao suprimiu: este teste deixou de exercitar o defeito");
	zassert_true(filtered < test_prof.abort_mc,
		     "a leitura filtrada (%d) ja passou do abort: idem", filtered);

	zassert_equal(reflow_run_tick(&run, &test_prof, 250, filtered, raw_mc),
		      REFLOW_RUN_ERR_OVERTEMP,
		      "forno a %d mC com o filtro segurando em %d mC e a corrida seguiu",
		      raw_mc, filtered);
}

/*
 * A barreira antiga nao pode ter enfraquecido: leitura filtrada acima do abort
 * continua abortando, mesmo com a bruta abaixo. E o caso de uma leitura travada
 * alto - nao e motivo para voltar a energizar.
 */
ZTEST(reflow_profile, test_abort_pela_leitura_filtrada_continua_valendo)
{
	struct reflow_run run;

	reflow_run_start(&run, &test_prof, 20000);
	zassert_equal(reflow_run_tick(&run, &test_prof, 250, 151000, 25000),
		      REFLOW_RUN_ERR_OVERTEMP,
		      "abort pela leitura filtrada deixou de valer");
}

ZTEST(reflow_profile, test_nominal_duration)
{
	zassert_equal(reflow_profile_nominal_ms(&test_prof), 40000,
		      "nominal duration mismatch");
}


/*
 * RFO-G12. O teto de uma simulação tem de vir do perfil. `host_sim` tinha 3600 s
 * literais e o perfil de bake soma 3780 s de nominal, então aquela execução não
 * podia terminar — a ferramenta reprovava um controle que rastreava com 0,45 °C
 * de erro rms.
 */
ZTEST(reflow_profile, test_max_ms_cobre_a_duracao_de_todo_perfil_embutido)
{
	const struct reflow_profile *p;
	uint8_t i;

	for (i = 0; (p = reflow_profile_get(i)) != NULL && i < 16; i++) {
		uint32_t nominal = reflow_profile_nominal_ms(p);
		uint32_t teto = reflow_profile_max_ms(p);

		zassert_true(teto > nominal,
			     "perfil %u (%s): teto %u nao passa do nominal %u",
			     i, p->name, teto, nominal);

		/* O teto e exatamente o ponto em que a maquina de estados ja
		 * declarou timeout: nominal mais a graca de cada estagio. A
		 * graca de um estagio de resfriamento e REFLOW_COOL_GRACE_MS e
		 * nao grace_ms (RFO-B04), entao a soma esperada e por estagio.
		 */
		uint32_t esperado = nominal;

		for (uint8_t s = 0; s < p->n_stages; s++) {
			esperado += p->stages[s].kind == REFLOW_STAGE_COOL ?
				REFLOW_COOL_GRACE_MS : p->grace_ms;
		}

		zassert_equal(teto, esperado,
			      "perfil %u (%s): teto %u nao e nominal + a graca de "
			      "cada estagio (esperado %u)",
			      i, p->name, teto, esperado);
	}
	zassert_true(i >= 4, "esperava ao menos 4 perfis embutidos, vi %u", i);
}

/*
 * A regressao concreta do a089dad, fixada como teste: existe perfil embutido
 * mais longo que o teto literal antigo. Enquanto isto valer, qualquer volta ao
 * numero fixo quebra aqui em vez de quebrar no host_sim de quem for ajustar
 * ganhos.
 */
ZTEST(reflow_profile, test_existe_perfil_mais_longo_que_o_teto_antigo)
{
	const uint32_t TETO_ANTIGO_MS = 3600U * 1000U;
	const struct reflow_profile *p;
	uint8_t i;
	bool achou = false;

	for (i = 0; (p = reflow_profile_get(i)) != NULL && i < 16; i++) {
		if (reflow_profile_nominal_ms(p) > TETO_ANTIGO_MS) {
			achou = true;
			zassert_true(reflow_profile_max_ms(p) > reflow_profile_nominal_ms(p),
				     "perfil %s cabe no nominal mas nao no teto", p->name);
		}
	}

	zassert_true(achou,
		     "nenhum perfil passa de %u ms; se um foi removido, este teste "
		     "perdeu o sentido e deve ser revisto, nao apagado",
		     TETO_ANTIGO_MS);
}

ZTEST_SUITE(reflow_profile, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------- command dispatch */

/*
 * RFO-B01. The dispatch used to be four unanchored strstr() calls, with
 * "id=start" tested first: any occurrence of that substring anywhere in the
 * query started a run, including inside the value of another parameter.
 */

static enum reflow_cmd_parse parse(const char *query, struct reflow_cmd *cmd)
{
	cmd->id = 0xFFU;
	cmd->arg = -1;
	return reflow_cmd_parse(query, cmd);
}

ZTEST(reflow_cmdparse, test_stop_is_not_hijacked_by_another_parameter)
{
	struct reflow_cmd cmd;

	zassert_equal(parse("id=stop&note=id=start", &cmd), REFLOW_CMD_PARSE_OK,
		      "a well formed stop must be accepted");
	zassert_equal(cmd.id, REFLOW_CMD_STOP,
		      "?id=stop&note=id=start started the oven (id=%u)", cmd.id);
}

ZTEST(reflow_cmdparse, test_key_must_match_whole)
{
	struct reflow_cmd cmd;

	/* "myid" is not "id", and "startle" is not "start". */
	zassert_equal(parse("myid=start", &cmd), REFLOW_CMD_PARSE_REJECT,
		      "a key ending in 'id' must not be taken for 'id'");
	zassert_equal(parse("id=startle", &cmd), REFLOW_CMD_PARSE_REJECT,
		      "the value must match the command in full");
	zassert_equal(parse("x=id=start", &cmd), REFLOW_CMD_PARSE_REJECT,
		      "a command hidden in another value must not be executed");
}

ZTEST(reflow_cmdparse, test_repeated_id_is_rejected)
{
	struct reflow_cmd cmd;

	/* Neither first-wins nor last-wins: an ambiguous oven command is an error. */
	zassert_equal(parse("id=start&id=clear", &cmd), REFLOW_CMD_PARSE_REJECT,
		      "a repeated id must not be resolved, it must be rejected");
	zassert_equal(parse("id=start&id=start", &cmd), REFLOW_CMD_PARSE_REJECT,
		      "a repeated id is ambiguous even when the values agree");

	/* ... but if a stop was one of the readings, the safe outcome is to stop. */
	zassert_equal(parse("id=start&id=stop", &cmd), REFLOW_CMD_PARSE_REJECT_STOP,
		      "stop must win over any other possible reading");
	zassert_equal(cmd.id, REFLOW_CMD_STOP, "ambiguous stop must still stop");
}

ZTEST(reflow_cmdparse, test_unknown_command_executes_nothing)
{
	struct reflow_cmd cmd;

	zassert_equal(parse("id=bake", &cmd), REFLOW_CMD_PARSE_REJECT,
		      "unknown command must be rejected");
	zassert_equal(cmd.id, 0xFFU, "nothing may be written on rejection");
}

ZTEST(reflow_cmdparse, test_empty_and_missing_query)
{
	struct reflow_cmd cmd;

	zassert_equal(parse(NULL, &cmd), REFLOW_CMD_PARSE_REJECT, "no query");
	zassert_equal(parse("", &cmd), REFLOW_CMD_PARSE_REJECT, "empty query");
	zassert_equal(parse("arg=2", &cmd), REFLOW_CMD_PARSE_REJECT, "no id at all");
}

ZTEST(reflow_cmdparse, test_id_without_a_value)
{
	struct reflow_cmd cmd;

	zassert_equal(parse("id", &cmd), REFLOW_CMD_PARSE_REJECT, "bare 'id'");
	zassert_equal(parse("id=", &cmd), REFLOW_CMD_PARSE_REJECT, "empty value");
	zassert_equal(parse("id=&arg=1", &cmd), REFLOW_CMD_PARSE_REJECT,
		      "empty value with another parameter");
}

ZTEST(reflow_cmdparse, test_percent_encoding_is_decoded)
{
	struct reflow_cmd cmd;

	zassert_equal(parse("id=%73tart", &cmd), REFLOW_CMD_PARSE_OK,
		      "%%73tart must decode to start");
	zassert_equal(cmd.id, REFLOW_CMD_START, "decoded start");

	zassert_equal(parse("%69d=st%6Fp", &cmd), REFLOW_CMD_PARSE_OK,
		      "the key is percent-decoded too");
	zassert_equal(cmd.id, REFLOW_CMD_STOP, "decoded stop");

	/* A broken escape is a malformed request, not a command. */
	zassert_equal(parse("id=%zztart", &cmd), REFLOW_CMD_PARSE_REJECT,
		      "invalid hex escape");
	zassert_equal(parse("id=star%7", &cmd), REFLOW_CMD_PARSE_REJECT,
		      "truncated escape");
	zassert_equal(parse("id=%00start", &cmd), REFLOW_CMD_PARSE_REJECT,
		      "an embedded NUL must not truncate the value");
}

ZTEST(reflow_cmdparse, test_profile_argument)
{
	struct reflow_cmd cmd;

	zassert_equal(parse("id=profile&arg=2", &cmd), REFLOW_CMD_PARSE_OK, "profile 2");
	zassert_equal(cmd.id, REFLOW_CMD_SELECT_PROFILE, "profile command");
	zassert_equal(cmd.arg, 2, "arg not parsed, got %d", cmd.arg);

	zassert_equal(parse("arg=2&id=profile", &cmd), REFLOW_CMD_PARSE_OK,
		      "parameter order must not matter");
	zassert_equal(cmd.arg, 2, "arg not parsed when it comes first");

	zassert_equal(parse("id=profile&arg=2x", &cmd), REFLOW_CMD_PARSE_REJECT,
		      "a non numeric arg is malformed");
	zassert_equal(parse("id=profile&arg=1&arg=2", &cmd), REFLOW_CMD_PARSE_REJECT,
		      "a repeated arg is ambiguous");
}

/*
 * RFO-B10. reflow_cmd_parse() aceitava qualquer indice ate 0xFFFF e o mandava
 * para o nucleo, que faz reflow_profile_get((uint8_t)cmd->arg): arg=256 truncava
 * para 0 e SELECIONAVA o perfil 0 — o SAC305, que vai a 245 degC — enquanto a API
 * respondia 204. Uma placa que devia rodar o bake de 120 degC rodava o perfil de
 * solda, com a UI mostrando o pedido como aceito.
 */
ZTEST(reflow_cmdparse, test_indice_de_perfil_fora_da_faixa_e_recusado)
{
	static const char *const queries[] = {
		"id=profile&arg=7",       /* acima da contagem, sem truncar */
		"id=profile&arg=256",     /* trunca para 0 em uint8_t: o caso do ticket */
		"id=profile&arg=65535",   /* o teto que o parse_index ja aceitava */
	};
	struct reflow_cmd cmd;

	/* Se a tabela crescer ate 7 perfis, o primeiro caso deixa de ser fora da
	 * faixa e o teste passaria sem exercitar nada. */
	zassert_true(reflow_profile_count() <= 7,
		     "esta tabela tem %u perfis: revise os indices deste teste",
		     reflow_profile_count());

	for (size_t i = 0; i < ARRAY_SIZE(queries); i++) {
		cmd.id = 0xFF;
		cmd.arg = -12345;
		zassert_equal(reflow_cmd_parse(queries[i], &cmd),
			      REFLOW_CMD_PARSE_REJECT,
			      "%s foi aceito", queries[i]);
		/* O criterio nao e so recusar: e nao deixar nada para o nucleo
		 * postar. As sentinelas tem de sobreviver a recusa. */
		zassert_equal(cmd.id, 0xFFU,
			      "%s escreveu id=%u sobre a sentinela",
			      queries[i], (unsigned int)cmd.id);
		zassert_equal(cmd.arg, -12345,
			      "%s escreveu arg=%d sobre a sentinela",
			      queries[i], cmd.arg);
	}
}

/* O sinal de menos nao e digito, entao ja era recusado. Ninguem guardava isso. */
ZTEST(reflow_cmdparse, test_indice_negativo_continua_recusado)
{
	struct reflow_cmd cmd;

	zassert_equal(reflow_cmd_parse("id=profile&arg=-1", &cmd),
		      REFLOW_CMD_PARSE_REJECT, "arg=-1 foi aceito");
}

/* Toda a faixa valida continua passando: o patch recusa, nao aperta demais. */
ZTEST(reflow_cmdparse, test_todo_indice_valido_continua_aceito)
{
	struct reflow_cmd cmd;

	for (uint8_t i = 0; i < reflow_profile_count(); i++) {
		char q[32];

		(void)snprintf(q, sizeof(q), "id=profile&arg=%u", i);
		cmd.arg = -1;
		zassert_equal(reflow_cmd_parse(q, &cmd), REFLOW_CMD_PARSE_OK,
			      "%s foi recusado", q);
		zassert_equal(cmd.arg, i, "%s virou arg=%d", q, cmd.arg);
		zassert_equal(cmd.id, REFLOW_CMD_SELECT_PROFILE, NULL);
	}
}

/*
 * O item que o Gerente marcou como "onde isso se perde por descuido": recusar um
 * stop e a unica falha que este endpoint nao pode ter, e um patch de validacao e
 * exatamente o lugar onde ela aparece.
 */
ZTEST(reflow_cmdparse, test_validacao_nao_engole_um_stop)
{
	struct reflow_cmd cmd;

	/* Ambigua E com indice fora da faixa: ainda tem de parar o forno. */
	cmd.id = 0xFF;
	zassert_equal(reflow_cmd_parse("id=stop&id=profile&arg=256", &cmd),
		      REFLOW_CMD_PARSE_REJECT_STOP,
		      "um stop foi engolido pela validacao de faixa");
	zassert_equal(cmd.id, REFLOW_CMD_STOP, "o comando devolvido nao e um stop");

	/* E um stop simples com um arg fora de faixa pendurado nao vira malformado:
	 * o arg nao e dele. */
	cmd.id = 0xFF;
	zassert_equal(reflow_cmd_parse("id=stop&arg=256", &cmd),
		      REFLOW_CMD_PARSE_OK,
		      "um arg que nao pertence ao comando o tornou invalido");
	zassert_equal(cmd.id, REFLOW_CMD_STOP, NULL);
	zassert_equal(cmd.arg, 0, "arg deveria ser zerado para um stop");
}

/*
 * RFO-B40. O irmao do B10: aquele fechou a porta do indice fora da faixa e esta
 * ficou aberta. "id=profile" sem arg nenhum devolvia OK com cmd.arg == 0, a API
 * respondia 204 e o nucleo trocava o perfil ativo para o indice 0 -- o SAC305,
 * que vai a 245 degC. O operador via o pedido aceito; o forno fazia outra coisa.
 *
 * A diferenca que este teste guarda e entre "nao pediu perfil nenhum" e "pediu o
 * perfil 0". Sao a mesma coisa para o parser antigo porque arg comeca em 0, e
 * sao coisas opostas para quem esta olhando a placa.
 */
ZTEST(reflow_cmdparse, test_perfil_sem_arg_e_recusado)
{
	static const char *const queries[] = {
		"id=profile",             /* o caso do ticket */
		"id=profile&argx=256",    /* mesma coisa com a chave errada */
		"arg=2&id=profile&argx=1" /* arg presente, mas nao e o do comando... */
	};
	struct reflow_cmd cmd;

	for (size_t i = 0; i < ARRAY_SIZE(queries) - 1; i++) {
		cmd.id = 0xFF;
		cmd.arg = -12345;
		zassert_equal(reflow_cmd_parse(queries[i], &cmd),
			      REFLOW_CMD_PARSE_REJECT,
			      "%s selecionou um perfil sem ninguem ter pedido",
			      queries[i]);
		zassert_equal(cmd.id, 0xFFU,
			      "%s escreveu id=%u sobre a sentinela",
			      queries[i], (unsigned int)cmd.id);
		zassert_equal(cmd.arg, -12345,
			      "%s escreveu arg=%d sobre a sentinela",
			      queries[i], cmd.arg);
	}

	/* ...e a ultima da lista tem arg de verdade: e valida, e continua sendo. */
	cmd.arg = -12345;
	zassert_equal(reflow_cmd_parse(queries[2], &cmd), REFLOW_CMD_PARSE_OK,
		      "%s foi recusada: uma chave desconhecida nao invalida o arg",
		      queries[2]);
	zassert_equal(cmd.arg, 2, "%s virou arg=%d", queries[2], cmd.arg);
}

/*
 * O outro lado da mesma linha: pedir o perfil 0 explicitamente continua valendo.
 * Sem esta asserção, "recusar quando arg falta" e "recusar o indice 0" passariam
 * pelo mesmo teste, e o segundo seria um forno que nao consegue mais rodar o
 * primeiro perfil da tabela.
 */
ZTEST(reflow_cmdparse, test_perfil_zero_pedido_de_proposito_continua_aceito)
{
	struct reflow_cmd cmd;

	cmd.arg = -12345;
	zassert_equal(reflow_cmd_parse("id=profile&arg=0", &cmd),
		      REFLOW_CMD_PARSE_OK, "arg=0 explicito foi recusado");
	zassert_equal(cmd.id, REFLOW_CMD_SELECT_PROFILE, NULL);
	zassert_equal(cmd.arg, 0, "arg=0 explicito virou arg=%d", cmd.arg);
}

/*
 * A exigencia de arg e do profile e de mais ninguem. Este e o teste que fica
 * VERDE sob a mutacao do item 4 e vermelho se o patch for largo demais: um
 * "todo comando precisa de arg" quebraria o stop, que e a unica falha que este
 * endpoint nao pode ter.
 */
ZTEST(reflow_cmdparse, test_exigencia_de_arg_nao_alcanca_os_outros_comandos)
{
	static const uint8_t esperado[] = {
		REFLOW_CMD_STOP, REFLOW_CMD_START, REFLOW_CMD_CLEAR_FAULT,
	};
	static const char *const queries[] = {
		"id=stop", "id=start", "id=clear",
	};
	struct reflow_cmd cmd;

	for (size_t i = 0; i < ARRAY_SIZE(queries); i++) {
		cmd.id = 0xFF;
		cmd.arg = -12345;
		zassert_equal(reflow_cmd_parse(queries[i], &cmd),
			      REFLOW_CMD_PARSE_OK,
			      "%s virou malformado por falta de arg", queries[i]);
		zassert_equal(cmd.id, esperado[i], "%s devolveu id=%u",
			      queries[i], (unsigned int)cmd.id);
		zassert_equal(cmd.arg, 0, "%s devolveu arg=%d", queries[i], cmd.arg);
	}

	/* Ambigua e sem arg: ainda tem de parar o forno. */
	cmd.id = 0xFF;
	zassert_equal(reflow_cmd_parse("id=stop&id=profile", &cmd),
		      REFLOW_CMD_PARSE_REJECT_STOP,
		      "um stop foi engolido pela exigencia de arg");
	zassert_equal(cmd.id, REFLOW_CMD_STOP, "o comando devolvido nao e um stop");
}

ZTEST_SUITE(reflow_cmdparse, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------ command authorisation */

/*
 * RFO-B02. Before the patch nothing between the request line and
 * reflow_cmd_post() was inspected, so `POST /api/cmd?id=start` from any page in
 * the browser started a 245 degC run. These exercise the gate that now stands
 * there. Requests are written out in full, CRLF and all, because the framing is
 * part of what is under test.
 */

#define TOKEN "s3gred0-de-bancada"

#define REQ(target, extra)                          \
	"POST " target " HTTP/1.1\r\n"               \
	"Host: 192.168.7.1\r\n"                      \
	extra                                        \
	"\r\n"

#define TOKEN_HDR "X-Reflow-Token: " TOKEN "\r\n"

ZTEST(reflow_httpgate, test_good_request_is_allowed)
{
	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start", TOKEN_HDR), TOKEN),
		      REFLOW_GATE_ALLOW, "a well formed authorised command must pass");
	zassert_is_null(reflow_gate_status(REFLOW_GATE_ALLOW),
			"ALLOW has no HTTP status of its own");
}

ZTEST(reflow_httpgate, test_token_must_be_present_and_exact)
{
	/* The drive-by from the ticket: no header at all. */
	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start", ""), TOKEN),
		      REFLOW_GATE_UNAUTHORIZED, "a command with no token started the oven");

	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    "X-Reflow-Token: nope\r\n"), TOKEN),
		      REFLOW_GATE_UNAUTHORIZED, "wrong token");

	/* Right prefix, short: this is what a length-blind compare lets through. */
	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    "X-Reflow-Token: s3gred0\r\n"), TOKEN),
		      REFLOW_GATE_UNAUTHORIZED, "a prefix of the token is not the token");

	/* Right prefix, long. */
	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    "X-Reflow-Token: " TOKEN "x\r\n"), TOKEN),
		      REFLOW_GATE_UNAUTHORIZED, "token plus a byte is not the token");

	/* Empty value. */
	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    "X-Reflow-Token:\r\n"), TOKEN),
		      REFLOW_GATE_UNAUTHORIZED, "empty token value");
}

ZTEST(reflow_httpgate, test_build_without_token_refuses_everything)
{
	/* Fail closed: never "accepts because no token was configured". */
	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start", TOKEN_HDR), ""),
		      REFLOW_GATE_DISABLED, "no token in the build must disable commands");
	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=stop", ""), ""),
		      REFLOW_GATE_DISABLED, "and it must disable them for everyone");
}

ZTEST(reflow_httpgate, test_read_only_routes_stay_open)
{
	zassert_equal(reflow_gate_check("GET / HTTP/1.1\r\nHost: x\r\n\r\n", TOKEN),
		      REFLOW_GATE_ALLOW, "the page must stay open");
	zassert_equal(reflow_gate_check("GET /api/state HTTP/1.1\r\n\r\n", TOKEN),
		      REFLOW_GATE_ALLOW, "telemetry cannot change the oven state");
	zassert_equal(reflow_gate_check("GET /api/events HTTP/1.1\r\n\r\n", TOKEN),
		      REFLOW_GATE_ALLOW, "the event stream must stay open");
	zassert_equal(reflow_gate_check("GET /api/profiles HTTP/1.1\r\n\r\n", TOKEN),
		      REFLOW_GATE_ALLOW, "the profile list must stay open");
}

ZTEST(reflow_httpgate, test_preflight_is_never_granted)
{
	/* A preflight that succeeds undoes the whole protection. */
	zassert_equal(reflow_gate_check("OPTIONS /api/cmd HTTP/1.1\r\n"
					"Host: 192.168.7.1\r\n"
					"Origin: http://evil.example\r\n"
					"Access-Control-Request-Method: POST\r\n"
					"Access-Control-Request-Headers: x-reflow-token\r\n"
					"\r\n", TOKEN),
		      REFLOW_GATE_METHOD_NOT_ALLOWED, "OPTIONS must not be answered");

	/* And it must be refused even on a build with no token, so the answer
	 * never depends on the secret. */
	zassert_equal(reflow_gate_check("OPTIONS /api/cmd HTTP/1.1\r\n"
					"Host: 192.168.7.1\r\n\r\n", ""),
		      REFLOW_GATE_METHOD_NOT_ALLOWED, "405 regardless of the token");

	zassert_equal(reflow_gate_check("GET /api/cmd?id=start HTTP/1.1\r\n"
					"Host: 192.168.7.1\r\n" TOKEN_HDR "\r\n", TOKEN),
		      REFLOW_GATE_METHOD_NOT_ALLOWED, "GET on the command endpoint");
}

ZTEST(reflow_httpgate, test_origin_must_match_host)
{
	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    "Origin: http://192.168.7.1\r\n" TOKEN_HDR),
					TOKEN),
		      REFLOW_GATE_ALLOW, "same origin must pass");

	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    "Origin: http://10.0.0.9\r\n" TOKEN_HDR),
					TOKEN),
		      REFLOW_GATE_FORBIDDEN, "cross origin command must be refused");

	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    "Origin: null\r\n" TOKEN_HDR), TOKEN),
		      REFLOW_GATE_FORBIDDEN, "an opaque origin is not the oven");

	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    "Origin: http://192.168.7.1.evil.example\r\n"
					    TOKEN_HDR), TOKEN),
		      REFLOW_GATE_FORBIDDEN, "a host that merely starts with ours");
}

ZTEST(reflow_httpgate, test_host_must_be_an_address_literal)
{
	/* DNS rebinding: a name can be re-pointed at the attacker, an address
	 * cannot. */
	zassert_equal(reflow_gate_check("POST /api/cmd?id=start HTTP/1.1\r\n"
					"Host: oven.local\r\n" TOKEN_HDR "\r\n", TOKEN),
		      REFLOW_GATE_FORBIDDEN, "a DNS name must be refused");

	zassert_equal(reflow_gate_check("POST /api/cmd?id=start HTTP/1.1\r\n"
					"Host: 192.168.7.1:8080\r\n" TOKEN_HDR "\r\n", TOKEN),
		      REFLOW_GATE_ALLOW, "a literal with a port is still a literal");

	/* 010.0.0.1 and 10.0.0.1 naming the same host is a second spelling. */
	zassert_equal(reflow_gate_check("POST /api/cmd?id=start HTTP/1.1\r\n"
					"Host: 010.168.7.1\r\n" TOKEN_HDR "\r\n", TOKEN),
		      REFLOW_GATE_FORBIDDEN, "octal-looking octet must be refused");

	zassert_equal(reflow_gate_check("POST /api/cmd?id=start HTTP/1.1\r\n"
					"Host: 192.168.7.256\r\n" TOKEN_HDR "\r\n", TOKEN),
		      REFLOW_GATE_FORBIDDEN, "octet above 255");

	zassert_equal(reflow_gate_check("POST /api/cmd?id=start HTTP/1.1\r\n"
					TOKEN_HDR "\r\n", TOKEN),
		      REFLOW_GATE_BAD_REQUEST, "HTTP/1.1 without Host");
}

ZTEST(reflow_httpgate, test_duplicate_headers_are_an_error)
{
	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    TOKEN_HDR TOKEN_HDR), TOKEN),
		      REFLOW_GATE_BAD_REQUEST, "two tokens is ambiguous");

	zassert_equal(reflow_gate_check("POST /api/cmd?id=start HTTP/1.1\r\n"
					"Host: 192.168.7.1\r\n"
					"Host: 192.168.7.1\r\n" TOKEN_HDR "\r\n", TOKEN),
		      REFLOW_GATE_BAD_REQUEST, "two Hosts is ambiguous");

	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    "Origin: http://192.168.7.1\r\n"
					    "Origin: http://192.168.7.1\r\n" TOKEN_HDR),
					TOKEN),
		      REFLOW_GATE_BAD_REQUEST, "two Origins is ambiguous");
}

ZTEST(reflow_httpgate, test_truncated_or_malformed_is_refused)
{
	/* No end of headers inside what we read: the Origin or a second token
	 * could be in the part we never saw. */
	zassert_equal(reflow_gate_check("POST /api/cmd?id=start HTTP/1.1\r\n"
					"Host: 192.168.7.1\r\n" TOKEN_HDR, TOKEN),
		      REFLOW_GATE_BAD_REQUEST, "no CRLFCRLF must fail closed");

	zassert_equal(reflow_gate_check("POST /api/cmd?id=start HTTP/1.1", TOKEN),
		      REFLOW_GATE_BAD_REQUEST, "request line not even finished");

	zassert_equal(reflow_gate_check("", TOKEN),
		      REFLOW_GATE_BAD_REQUEST, "empty request");
	zassert_equal(reflow_gate_check(NULL, TOKEN),
		      REFLOW_GATE_BAD_REQUEST, "no request at all");

	/* A header line with no colon. */
	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    "this-is-not-a-header\r\n" TOKEN_HDR),
					TOKEN),
		      REFLOW_GATE_BAD_REQUEST, "line without a colon");

	/* Whitespace before the colon is a smuggling primitive, not a typo. */
	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    "X-Reflow-Token : " TOKEN "\r\n"), TOKEN),
		      REFLOW_GATE_BAD_REQUEST, "space before the colon");
}

ZTEST(reflow_httpgate, test_header_name_case_and_whitespace)
{
	zassert_equal(reflow_gate_check("POST /api/cmd?id=start HTTP/1.1\r\n"
					"HOST: 192.168.7.1\r\n"
					"x-REFLOW-token: " TOKEN "\r\n\r\n", TOKEN),
		      REFLOW_GATE_ALLOW, "header names are case insensitive");

	zassert_equal(reflow_gate_check("POST /api/cmd?id=start HTTP/1.1\r\n"
					"Host:\t 192.168.7.1 \r\n"
					"X-Reflow-Token:   " TOKEN "\t\r\n\r\n", TOKEN),
		      REFLOW_GATE_ALLOW, "surrounding whitespace must be ignored");

	/* The value itself is not case folded: a token is a secret, not a name. */
	zassert_equal(reflow_gate_check(REQ("/api/cmd?id=start",
					    "X-Reflow-Token: S3GRED0-DE-BANCADA\r\n"),
					TOKEN),
		      REFLOW_GATE_UNAUTHORIZED, "the token value is case sensitive");
}

ZTEST(reflow_httpgate, test_every_refusal_has_a_status)
{
	zassert_str_equal(reflow_gate_status(REFLOW_GATE_BAD_REQUEST), "400 Bad Request");
	zassert_str_equal(reflow_gate_status(REFLOW_GATE_UNAUTHORIZED), "401 Unauthorized");
	zassert_str_equal(reflow_gate_status(REFLOW_GATE_FORBIDDEN), "403 Forbidden");
	zassert_str_equal(reflow_gate_status(REFLOW_GATE_METHOD_NOT_ALLOWED),
			  "405 Method Not Allowed");
	zassert_str_equal(reflow_gate_status(REFLOW_GATE_DISABLED),
			  "503 Service Unavailable");
}

/*
 * Truncation sweep: the server does a single recv() into a 512 byte buffer, so a
 * request cut at an arbitrary offset is the normal case, not the exotic one.
 * Every prefix of a valid request must be refused, and none may read past the
 * terminator.
 */
ZTEST(reflow_httpgate, test_no_prefix_of_a_valid_request_is_allowed)
{
	static const char full[] =
		"POST /api/cmd?id=start HTTP/1.1\r\n"
		"Host: 192.168.7.1\r\n"
		"Origin: http://192.168.7.1\r\n"
		"X-Reflow-Token: " TOKEN "\r\n"
		"\r\n";
	char buf[sizeof(full)];
	size_t n;

	zassert_equal(reflow_gate_check(full, TOKEN), REFLOW_GATE_ALLOW,
		      "the complete request must pass");

	for (n = 0; n < sizeof(full) - 1U; n++) {
		memcpy(buf, full, n);
		buf[n] = '\0';
		zassert_not_equal(reflow_gate_check(buf, TOKEN), REFLOW_GATE_ALLOW,
				  "prefix of %zu bytes was allowed", n);
	}
}

/*
 * Single byte corruption sweep. Deterministic: every position gets four
 * substitutions chosen to hit the parser's structure (NUL, CR, colon, space).
 * The point is not that each one is refused — corrupting the query string still
 * leaves a valid authorised request — but that none of them reads out of bounds
 * or loops forever.
 */
ZTEST(reflow_httpgate, test_single_byte_corruption_is_survivable)
{
	static const char full[] =
		"POST /api/cmd?id=start HTTP/1.1\r\n"
		"Host: 192.168.7.1\r\n"
		"Origin: http://192.168.7.1\r\n"
		"X-Reflow-Token: " TOKEN "\r\n"
		"\r\n";
	static const char pokes[] = { '\0', '\r', ':', ' ' };
	char buf[sizeof(full)];
	size_t i, k;
	int allowed = 0;

	for (i = 0; i < sizeof(full) - 1U; i++) {
		for (k = 0; k < sizeof(pokes); k++) {
			memcpy(buf, full, sizeof(full));
			buf[i] = pokes[k];
			if (reflow_gate_check(buf, TOKEN) == REFLOW_GATE_ALLOW) {
				allowed++;
			}
		}
	}

	/*
	 * Corrupting a byte must never turn an unauthorised request into an
	 * authorised one; here the request starts out valid, so some corruptions
	 * legitimately still pass (anything inside the query string). What must
	 * hold is that the sweep completes without a fault.
	 */
	zassert_true(allowed >= 0, "sweep completed");
}

ZTEST_SUITE(reflow_httpgate, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------- temperature guard rails */

#define LIMIT_MC 270000 /* CONFIG_REFLOW_ABS_MAX_TEMP_C default, in milli-degC */

/*
 * RFO-T04. The filter may lag a real step, it may not outlast it. Scenario A
 * of RFO-B05: stable at 240 degC, the thermocouple is reseated and now reads
 * 25 degC for good. Reporting 240 degC after eight real samples of 25 degC is
 * the defect.
 */
ZTEST(reflow_tempguard, test_persistent_step_is_eventually_accepted)
{
	struct reflow_spike s;
	int32_t out = 0;
	int i;

	reflow_spike_reset(&s);

	for (i = 0; i < 5; i++) {
		zassert_equal(reflow_spike_filter(&s, 240000, &out), REFLOW_SPIKE_ACCEPT,
			      "a steady reading is not a spike");
		zassert_equal(out, 240000, "steady reading altered");
	}

	/* The suppression window is allowed, and it is bounded. */
	for (i = 0; i < REFLOW_SPIKE_MAX_REJECTS - 1; i++) {
		zassert_equal(reflow_spike_filter(&s, 25000, &out), REFLOW_SPIKE_REJECT,
			      "sample %d should still be treated as a spike", i);
		zassert_equal(out, 240000, "rejected sample must report the last value");
	}

	zassert_equal(reflow_spike_filter(&s, 25000, &out), REFLOW_SPIKE_FORCED,
		      "the %dth consecutive jumping sample must be believed, i.e. after "
		      "%d suppressed ones",
		      REFLOW_SPIKE_MAX_REJECTS, REFLOW_SPIKE_MAX_REJECTS - 1);
	zassert_equal(out, 25000, "the new value must be reported, not suppressed");

	/* And it stays believed: no oscillation back to the stale value. */
	for (i = 0; i < 8; i++) {
		zassert_equal(reflow_spike_filter(&s, 25000, &out), REFLOW_SPIKE_ACCEPT,
			      "the accepted value is now the reference");
		zassert_equal(out, 25000, "reading stuck at the pre-step value");
	}
}

/*
 * RFO-T04, scenario B: a real 200 degC/s ramp is 50 degC per 250 ms sample, so
 * every single sample looks like a spike. The reported temperature must track
 * the oven with bounded lag; the defect reports 25 degC while the oven passes
 * 275 degC.
 */
ZTEST(reflow_tempguard, test_real_ramp_is_tracked_with_bounded_lag)
{
	struct reflow_spike s;
	int32_t out = 0;
	int32_t raw;
	int run = 0;

	reflow_spike_reset(&s);
	(void)reflow_spike_filter(&s, 25000, &out);

	/*
	 * The invariant is a bound on how many samples in a row may be
	 * suppressed, not on the temperature error: the error depends on how
	 * fast the oven happens to move, the suppression window does not.
	 */
	for (raw = 75000; raw <= 325000; raw += 50000) {
		if (reflow_spike_filter(&s, raw, &out) == REFLOW_SPIKE_REJECT) {
			run++;
			zassert_true(run <= REFLOW_SPIKE_MAX_REJECTS - 1,
				     "%d samples suppressed in a row at %d mC; the bound is %d",
				     run, raw, REFLOW_SPIKE_MAX_REJECTS - 1);
			zassert_equal(out, s.last_mc,
				      "a suppressed sample must report the last believed value");
		} else {
			run = 0;
			zassert_equal(out, raw, "an accepted sample must be reported as read");
		}
	}

	zassert_equal(run, 0, "the ramp must not end inside a suppression window");
	zassert_true(out >= LIMIT_MC,
		     "a runaway ramp must be visible to the controller, reported %d mC",
		     out);
}

/*
 * The complement of the ramp above: a step exactly at the threshold is legal,
 * so a fast-but-plausible rise must never be suppressed at all. Without this,
 * raising REFLOW_SPIKE_MAX_STEP_MC would look free.
 */
ZTEST(reflow_tempguard, test_steep_but_legal_steps_are_never_suppressed)
{
	struct reflow_spike s;
	int32_t out = 0;
	int32_t raw;

	reflow_spike_reset(&s);
	(void)reflow_spike_filter(&s, 25000, &out);

	for (raw = 25000 + REFLOW_SPIKE_MAX_STEP_MC; raw <= 385000;
	     raw += REFLOW_SPIKE_MAX_STEP_MC) {
		zassert_equal(reflow_spike_filter(&s, raw, &out), REFLOW_SPIKE_ACCEPT,
			      "a step of exactly %d mC is inside the window, not a spike",
			      REFLOW_SPIKE_MAX_STEP_MC);
		zassert_equal(out, raw, "no lag is allowed on a legal step");
	}
}

/*
 * RFO-B05, availability scenario: the thermocouple falls on the element, reads
 * 400 degC, then returns to air. The step back down is a spike too, so the
 * reading latches high and FAULT_OVERTEMP reappears the instant it is cleared.
 */
ZTEST(reflow_tempguard, test_recovers_from_a_reading_latched_high)
{
	struct reflow_spike s;
	int32_t out = 0;
	int i;

	reflow_spike_reset(&s);
	(void)reflow_spike_filter(&s, 400000, &out);
	zassert_equal(out, 400000, "the first sample has nothing to be compared against");

	for (i = 0; i < REFLOW_SPIKE_MAX_REJECTS + 1; i++) {
		(void)reflow_spike_filter(&s, 25000, &out);
	}

	zassert_equal(out, 25000, "the oven must become usable again without a power cycle");
}

/*
 * The fix must not cost the filter its job: an isolated spike between two good
 * samples is still rejected, and does not accumulate towards the limit.
 */
ZTEST(reflow_tempguard, test_isolated_spikes_are_still_rejected)
{
	struct reflow_spike s;
	int32_t out = 0;
	int i;

	reflow_spike_reset(&s);
	(void)reflow_spike_filter(&s, 100000, &out);

	for (i = 0; i < 10; i++) {
		zassert_equal(reflow_spike_filter(&s, 200000, &out), REFLOW_SPIKE_REJECT,
			      "isolated spike %d was let through", i);
		zassert_equal(out, 100000, "spike leaked into the reported value");

		zassert_equal(reflow_spike_filter(&s, 100000, &out), REFLOW_SPIKE_ACCEPT,
			      "the good sample after a spike must be accepted");
		zassert_equal(out, 100000, "good sample altered");
	}
}

/*
 * RFO-T05. The absolute cut-out must be independent of the spike rejector:
 * the whole point of a backstop is that it does not share the input path with
 * the loop it backs up. Here the filter is legitimately suppressing the step
 * and the raw sample is already past the limit.
 */
ZTEST(reflow_tempguard, test_backstop_trips_on_the_raw_sample)
{
	struct reflow_spike s;
	int32_t out = 0;
	enum reflow_spike_result r;

	reflow_spike_reset(&s);
	(void)reflow_spike_filter(&s, 240000, &out);

	r = reflow_spike_filter(&s, 290000, &out);
	zassert_equal(r, REFLOW_SPIKE_REJECT, "one 50 degC step is still a spike");
	zassert_equal(out, 240000, "the control loop sees the filtered value");

	zassert_true(reflow_overtemp_tripped(out, 290000, LIMIT_MC),
		     "the element stays energised: the cut-out is blind to the raw %d mC",
		     290000);
}

ZTEST(reflow_tempguard, test_backstop_boundaries)
{
	/* Nothing above the limit: no trip. */
	zassert_false(reflow_overtemp_tripped(269999, 269999, LIMIT_MC),
		      "tripped below the limit");

	/* At the limit, both ways: controller.c compares with >=. */
	zassert_true(reflow_overtemp_tripped(LIMIT_MC, 25000, LIMIT_MC),
		     "filtered value at the limit must trip");
	zassert_true(reflow_overtemp_tripped(25000, LIMIT_MC, LIMIT_MC),
		     "raw value at the limit must trip");

	/* A reading latched high by the filter must keep tripping. */
	zassert_true(reflow_overtemp_tripped(400000, 25000, LIMIT_MC),
		     "a stale high reading is not a reason to re-energise");
}

ZTEST_SUITE(reflow_tempguard, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------- profiles_json */

/*
 * RFO-B09. httpd.c built the /api/profiles body by accumulating the return of
 * snprintk(), which is the length that WOULD have been written. One long name
 * past the buffer and the next call got (size_t)(sizeof(body) - n) with
 * n > sizeof(body) - an unsigned subtraction landing near SIZE_MAX - into a
 * destination already out of bounds, and the same n went to send_all() as the
 * length. Overflow on the stack, on a path reachable from the network.
 *
 * The formatter is a pure function now, so the case that used to be
 * unreachable-with-three-short-names is one array away from being tested.
 */

/* A canary either side of the buffer: any write outside is visible. */
struct guarded {
	char before[16];
	char body[512];
	char after[16];
};

static void guarded_init(struct guarded *g)
{
	memset(g, 0xAA, sizeof(*g));
}

static void guarded_check(const struct guarded *g, size_t cap)
{
	for (size_t i = 0; i < sizeof(g->before); i++) {
		zassert_equal((unsigned char)g->before[i], 0xAAU,
			      "wrote %zu bytes before the buffer", sizeof(g->before) - i);
	}
	for (size_t i = 0; i < sizeof(g->after); i++) {
		zassert_equal((unsigned char)g->after[i], 0xAAU,
			      "wrote past the end of the buffer (+%zu)", i);
	}
	/* Anything past cap inside body must also be untouched. */
	for (size_t i = cap; i < sizeof(g->body); i++) {
		zassert_equal((unsigned char)g->body[i], 0xAAU,
			      "wrote past cap, at body[%zu]", i);
	}
}

/* Profile table the test owns, so no dependency on the built-in one. */
static struct reflow_profile fake_profiles[8];
static uint8_t fake_count;

static const struct reflow_profile *fake_lookup(uint8_t idx)
{
	return idx < fake_count ? &fake_profiles[idx] : NULL;
}

static void set_profiles(uint8_t n, const char *const *names)
{
	zassert_true(n <= ARRAY_SIZE(fake_profiles), "test wants too many profiles");
	for (uint8_t i = 0; i < n; i++) {
		fake_profiles[i].name = names[i];
	}
	fake_count = n;
}

ZTEST(reflow_profiles_json, test_short_list_is_exact)
{
	static const char *const names[] = { "SAC305", "Sn63Pb37", "bake" };
	struct guarded g;
	size_t n;

	guarded_init(&g);
	set_profiles(3, names);

	n = reflow_profiles_json(g.body, sizeof(g.body), fake_lookup, 3);

	zassert_equal(n, strlen(g.body), "returned length does not match the string");
	zassert_str_equal(g.body, "{\"profiles\":[\"SAC305\",\"Sn63Pb37\",\"bake\"]}",
			  "body was %s", g.body);
	guarded_check(&g, sizeof(g.body));
}

ZTEST(reflow_profiles_json, test_empty_list_is_valid_json)
{
	struct guarded g;
	size_t n;

	guarded_init(&g);
	fake_count = 0;

	n = reflow_profiles_json(g.body, sizeof(g.body), fake_lookup, 0);

	zassert_str_equal(g.body, "{\"profiles\":[]}", "body was %s", g.body);
	zassert_equal(n, strlen(g.body), NULL);
	guarded_check(&g, sizeof(g.body));
}

/*
 * The assertion that goes red without the patch. Eight names of 40 characters
 * blow well past the 256 bytes httpd.c hands in, which is exactly the condition
 * that made the old code compute sizeof(body) - n near SIZE_MAX.
 */
ZTEST(reflow_profiles_json, test_overflow_never_leaves_the_buffer)
{
	static const char *const names[] = {
		"0123456789012345678901234567890123456789",
		"1123456789012345678901234567890123456789",
		"2123456789012345678901234567890123456789",
		"3123456789012345678901234567890123456789",
		"4123456789012345678901234567890123456789",
		"5123456789012345678901234567890123456789",
		"6123456789012345678901234567890123456789",
		"7123456789012345678901234567890123456789",
	};
	const size_t cap = 256;
	struct guarded g;
	size_t n;

	guarded_init(&g);
	set_profiles(8, names);

	n = reflow_profiles_json(g.body, cap, fake_lookup, 8);

	zassert_true(n < cap, "returned %zu for a %zu byte buffer", n, cap);
	zassert_equal(g.body[n], '\0', "the body is not terminated at the length returned");
	zassert_equal(n, strlen(g.body), "returned length counts bytes that were not written");
	guarded_check(&g, cap);
}

ZTEST(reflow_profiles_json, test_truncation_is_explicit_and_still_json)
{
	static const char *const names[] = {
		"0123456789012345678901234567890123456789",
		"1123456789012345678901234567890123456789",
		"2123456789012345678901234567890123456789",
		"3123456789012345678901234567890123456789",
		"4123456789012345678901234567890123456789",
		"5123456789012345678901234567890123456789",
		"6123456789012345678901234567890123456789",
		"7123456789012345678901234567890123456789",
	};
	struct guarded g;
	size_t n;

	guarded_init(&g);
	set_profiles(8, names);

	n = reflow_profiles_json(g.body, 256, fake_lookup, 8);

	/* Valid JSON, closed, and honest about being short. */
	zassert_equal(g.body[0], '{', "body does not open an object");
	zassert_equal(g.body[n - 1], '}', "body does not close: %s", g.body);
	zassert_not_null(strstr(g.body, "\"truncated\":true"),
			 "a short list must say so: %s", g.body);
	/* Never half a name: the last entry before the tail is closed. */
	zassert_not_null(strstr(g.body, "\"]"), "an entry was cut mid-string: %s", g.body);
}

ZTEST(reflow_profiles_json, test_quote_and_backslash_are_escaped)
{
	static const char *const names[] = { "he said \"hi\"", "back\\slash" };
	struct guarded g;
	size_t n;

	guarded_init(&g);
	set_profiles(2, names);

	n = reflow_profiles_json(g.body, sizeof(g.body), fake_lookup, 2);

	zassert_str_equal(g.body,
			  "{\"profiles\":[\"he said \\\"hi\\\"\",\"back\\\\slash\"]}",
			  "unescaped name reaches the wire: %s", g.body);
	zassert_equal(n, strlen(g.body), NULL);
	guarded_check(&g, sizeof(g.body));
}

ZTEST(reflow_profiles_json, test_buffer_too_small_writes_nothing)
{
	static const char *const names[] = { "SAC305" };
	struct guarded g;
	size_t n;

	guarded_init(&g);
	set_profiles(1, names);

	n = reflow_profiles_json(g.body, REFLOW_PROFILES_JSON_MIN - 1U, fake_lookup, 1);

	zassert_equal(n, 0, "claimed to write %zu bytes into a buffer that cannot hold a body", n);
	zassert_equal(g.body[0], '\0', "left the buffer non-empty while returning 0");
	guarded_check(&g, REFLOW_PROFILES_JSON_MIN - 1U);
}

/*
 * Every cap from too-small to comfortable, with names that overflow most of
 * them. One of these sizes is the boundary where the tail stops fitting, and a
 * clamp that is off by one shows up here rather than in a browser.
 */
ZTEST(reflow_profiles_json, test_every_buffer_size_is_safe)
{
	static const char *const names[] = {
		"0123456789012345678901234567890123456789",
		"1123456789012345678901234567890123456789",
		"2123456789012345678901234567890123456789",
	};

	set_profiles(3, names);

	for (size_t cap = 1; cap <= 200; cap++) {
		struct guarded g;
		size_t n;

		guarded_init(&g);
		n = reflow_profiles_json(g.body, cap, fake_lookup, 3);

		zassert_true(n < cap, "cap %zu: returned %zu", cap, n);
		zassert_equal(n, strlen(g.body), "cap %zu: length disagrees with the string", cap);
		if (n > 0) {
			zassert_equal(g.body[0], '{', "cap %zu: not an object", cap);
			zassert_equal(g.body[n - 1], '}', "cap %zu: not closed", cap);
		}
		guarded_check(&g, cap);
	}
}

ZTEST_SUITE(reflow_profiles_json, NULL, NULL, NULL, NULL, NULL);

/* --------------------------------------------------------- sendbudget */

/*
 * RFO-B07. httpd.c has one thread and called zsock_send() on a blocking socket.
 * A client that asks for the 6 KB page and never reads it fills the TCP send
 * buffer, send_all() parks, the thread never returns to zsock_poll(), and the
 * event stream dies for EVERY client - with the remote Stop button along with
 * it, while the oven is at 245 degC.
 *
 * What the budget decides is not "is this socket writable" - that is the
 * socket's job - but "has this client gone long enough without accepting a byte
 * that we should give up on it". That is a function of two instants, so it is
 * tested here instead of on a network.
 */

#define BUDGET_MS 500

ZTEST(reflow_sendbudget, test_a_client_that_keeps_reading_is_never_dropped)
{
	struct reflow_send_budget b;
	int64_t now = 1000;

	reflow_send_budget_start(&b, now, BUDGET_MS);

	/*
	 * Slow but alive: a byte every 400 ms for a minute. Total time on the
	 * connection is 120x the budget, and it must survive every check -
	 * the budget measures silence, not duration.
	 */
	for (int i = 0; i < 150; i++) {
		now += 400;
		zassert_false(reflow_send_budget_expired(&b, now),
			      "dropped a client that was still accepting bytes, at %lld ms",
			      (long long)now);
		reflow_send_budget_progress(&b, now);
	}
}

ZTEST(reflow_sendbudget, test_a_client_that_stops_reading_is_dropped)
{
	struct reflow_send_budget b;

	reflow_send_budget_start(&b, 1000, BUDGET_MS);

	zassert_false(reflow_send_budget_expired(&b, 1000), "expired immediately");
	zassert_false(reflow_send_budget_expired(&b, 1000 + BUDGET_MS),
		      "expired at the budget: the last allowed instant must survive");
	zassert_true(reflow_send_budget_expired(&b, 1000 + BUDGET_MS + 1),
		     "a silent client was not dropped one ms past the budget");
}

/*
 * The second item of the acceptance criteria, and the one a careless fix gets
 * wrong: a legitimate event stream sits open for hours sending nothing. It must
 * be impossible for this path to close it.
 */
ZTEST(reflow_sendbudget, test_an_idle_stream_is_never_dropped)
{
	struct reflow_send_budget b;

	reflow_send_budget_start(&b, 1000, BUDGET_MS);
	reflow_send_budget_done(&b);

	/* A whole day later, with no send in progress. */
	zassert_false(reflow_send_budget_expired(&b, 1000 + 86400000LL),
		      "an idle event stream was closed by the send deadline");
}

ZTEST(reflow_sendbudget, test_progress_restarts_the_clock)
{
	struct reflow_send_budget b;

	reflow_send_budget_start(&b, 0, BUDGET_MS);

	/* Almost out of time, then one byte moves. */
	zassert_false(reflow_send_budget_expired(&b, BUDGET_MS), NULL);
	reflow_send_budget_progress(&b, BUDGET_MS);

	/* The clock restarted: the old deadline no longer applies. */
	zassert_false(reflow_send_budget_expired(&b, BUDGET_MS + BUDGET_MS),
		      "progress did not restart the no-progress clock");
	zassert_true(reflow_send_budget_expired(&b, BUDGET_MS + BUDGET_MS + 1),
		     "the restarted clock never expires");
}

ZTEST(reflow_sendbudget, test_a_disabled_budget_never_expires)
{
	struct reflow_send_budget b;

	reflow_send_budget_start(&b, 0, 0);
	zassert_false(reflow_send_budget_expired(&b, 86400000LL),
		      "a budget of 0 must mean no deadline, not an instant one");

	reflow_send_budget_start(&b, 0, -1);
	zassert_false(reflow_send_budget_expired(&b, 86400000LL),
		      "a negative budget must not arm the deadline");
}

/*
 * A clock that goes backwards must not look like elapsed time. Keeping a client
 * one round too long is a slow page; dropping one wrongly is a browser that
 * loses the oven's telemetry.
 */
ZTEST(reflow_sendbudget, test_a_clock_that_goes_backwards_keeps_the_client)
{
	struct reflow_send_budget b;

	reflow_send_budget_start(&b, 100000, BUDGET_MS);
	zassert_false(reflow_send_budget_expired(&b, 0),
		      "time going backwards was read as the budget being spent");
}


/*
 * RFO-B08. Um cliente que abre a conexao e nao envia nada nunca fica legivel,
 * entao o serve() nunca roda e o client_close() nunca acontece. Quatro deles
 * ("nc <ip> 80" quatro vezes) tomam os quatro slots, e todo accept() seguinte e
 * fechado na chegada: a UI web, e com ela o Parar remoto, some ate o reset da
 * placa — possivelmente com o forno em ciclo.
 *
 * Este prazo e o irmao do de envio e nao o mesmo: aquele mede o cliente
 * enquanto o servidor FALA com ele; este mede um que nunca disse nada. Os dois
 * dividem a aritmetica e nao a instancia.
 */

#define IDLE_MS 5000

ZTEST(reflow_sendbudget, test_conexao_muda_perde_o_slot)
{
	struct reflow_idle_budget b;

	reflow_idle_budget_start(&b, 1000, IDLE_MS);

	zassert_false(reflow_idle_budget_expired(&b, 1000), "expirou na hora do accept");
	zassert_false(reflow_idle_budget_expired(&b, 1000 + IDLE_MS),
		      "expirou no limite: o ultimo instante permitido tem de sobreviver");
	zassert_true(reflow_idle_budget_expired(&b, 1000 + IDLE_MS + 1),
		     "conexao calada manteve o slot depois do prazo");
}

/*
 * O item que troca um defeito de disponibilidade por outro se for feito errado:
 * um SSE estabelecido fica ocioso por definicao e nao pode ser fechado por este
 * caminho, nunca.
 */
ZTEST(reflow_sendbudget, test_um_sse_estabelecido_nunca_e_fechado_pelo_prazo)
{
	struct reflow_idle_budget b;

	reflow_idle_budget_start(&b, 1000, IDLE_MS);
	reflow_idle_budget_disarm(&b);   /* virou event stream */

	zassert_false(reflow_idle_budget_expired(&b, 1000 + 86400000LL),
		      "um event stream foi derrubado por ociosidade depois de um dia");
}

/* Requisicao que chegou dentro do prazo: desarmado, e nunca mais fechado. */
ZTEST(reflow_sendbudget, test_requisicao_atendida_desarma_o_prazo)
{
	struct reflow_idle_budget b;

	reflow_idle_budget_start(&b, 0, IDLE_MS);
	zassert_false(reflow_idle_budget_expired(&b, IDLE_MS - 1), "expirou cedo demais");

	reflow_idle_budget_disarm(&b);
	zassert_false(reflow_idle_budget_expired(&b, IDLE_MS * 100),
		      "conexao ja atendida ainda pode ser fechada pelo prazo de accept");
}

ZTEST(reflow_sendbudget, test_prazo_de_ociosidade_desligado_nunca_expira)
{
	struct reflow_idle_budget b;

	reflow_idle_budget_start(&b, 0, 0);
	zassert_false(reflow_idle_budget_expired(&b, 86400000LL),
		      "orcamento 0 tem de significar sem prazo, nao prazo instantaneo");
}

/* Mesma regra do prazo de envio, e de proposito: relogio para tras mantem. */
ZTEST(reflow_sendbudget, test_prazo_de_ociosidade_com_relogio_para_tras)
{
	struct reflow_idle_budget b;

	reflow_idle_budget_start(&b, 100000, IDLE_MS);
	zassert_false(reflow_idle_budget_expired(&b, 0),
		      "tempo andando para tras foi lido como prazo gasto");
}

/*
 * Os dois prazos tem de concordar sobre o que "tempo demais" significa. Sao
 * tipos diferentes com nomes honestos, mas uma aritmetica so; se alguem um dia
 * duplicar a conta, este teste e que percebe.
 */
ZTEST(reflow_sendbudget, test_os_dois_prazos_concordam_no_limite)
{
	struct reflow_send_budget snd;
	struct reflow_idle_budget idle;

	reflow_send_budget_start(&snd, 0, 1000);
	reflow_idle_budget_start(&idle, 0, 1000);

	for (int64_t t = 0; t <= 1002; t++) {
		zassert_equal(reflow_send_budget_expired(&snd, t),
			      reflow_idle_budget_expired(&idle, t),
			      "os dois prazos discordam em t=%lld ms", (long long)t);
	}
}

ZTEST_SUITE(reflow_sendbudget, NULL, NULL, NULL, NULL, NULL);
