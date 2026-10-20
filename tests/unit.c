#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "awards.h"
#include "db.h"
#include "msg.h"
#include "params.h"
#include "util.h"

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
			failures++;                                            \
		}                                                              \
	} while (0)

static void check_str(const char *file, int line, char *got, const char *want)
{
	if (!got || strcmp(got, want)) {
		fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", file, line, got ? got : "(null)", want);
		failures++;
	}
	free(got);
}
#define CHECK_STR(got, want) check_str(__FILE__, __LINE__, (got), (want))

static void test_params(void)
{
	CHECK_STR(form_get("r=login2&u=Some+One&p=a%26b", "u"), "Some One");
	CHECK_STR(form_get("r=login2&u=x&p=a%26b", "p"), "a&b");
	CHECK(form_get("r=login2&u=x", "t") == NULL);
	CHECK(form_get("ru=1", "r") == NULL);
	CHECK_STR(form_get("a=1&a=2", "a"), "1");
	CHECK_STR(form_get("bad=%zz", "bad"), "%zz");
	CHECK_STR(form_set("r=x&t=old&u=y", "t", "n/w"), "r=x&t=n%2Fw&u=y");
	CHECK_STR(form_set("r=x", "o", "5"), "r=x&o=5");
	CHECK_STR(form_set("", "o", "5"), "o=5");
	CHECK_STR(form_set("t=1&t=2", "t", "3"), "t=3");
}

static void test_replay(void)
{
	const char *award = "r=awardachievement&u=Bob&t=old&a=123&h=1&m=abc&v=orig";
	const char *lb = "r=submitlbentry&u=Bob&t=old&i=77&s=-5&v=orig";
	int64_t q = ACHD_BUILD_EPOCH + 100;

	/* md5("123" "Bob" "1" "123" "50") */
	CHECK_STR(award_replay_body(award, "awardachievement", q, q + 50, "new"),
		  "r=awardachievement&u=Bob&t=new&a=123&h=1&m=abc&v=6eed1697e2db933f7944fae9574eeaf1&o=50");
	/* The emulator's own o (time between unlock and its first attempt) adds on. */
	CHECK_STR(award_replay_body("r=awardachievement&u=Bob&a=123&h=1&o=10", "awardachievement", q, q + 50, NULL),
		  "r=awardachievement&u=Bob&a=123&h=1&o=60&v=beb52ffe8a8ef872dac2710a17b3f363");
	CHECK_STR(award_replay_body(lb, "submitlbentry", q, q + 50, "new"),
		  "r=submitlbentry&u=Bob&t=new&i=77&s=-5&v=bd5d347ee75ad1d390bf4d4dd00e80c8&o=50");
	CHECK_STR(award_replay_body(award, "awardachievement", q, q + 30LL * 86400, NULL),
		  "r=awardachievement&u=Bob&t=old&a=123&h=1&m=abc&v=cf4bac118738a419a0c4dc0a0b29e0d1&o=1209600");
	/* Untrusted clock at queue time: only the token changes. */
	CHECK_STR(award_replay_body(award, "awardachievement", 0, q + 50, "new"),
		  "r=awardachievement&u=Bob&t=new&a=123&h=1&m=abc&v=orig");
	/* Clock went backwards. */
	CHECK_STR(award_replay_body(award, "awardachievement", q, q - 10, NULL), award);
}

struct stale_ctx {
	int n;
	bool saw;
};

static void stale_cb(const struct cache_key *k, const struct cache_meta *m, void *priv)
{
	struct stale_ctx *c = priv;

	c->n++;
	c->saw = !strcmp(k->action, "patch") && !strcmp(k->scope, "12") && !strcmp(k->user, "bob") &&
		 !k->hardcore && !strcmp(m->form, "r=patch&u=bob&g=12&t=") && m->game == 12;
}

static void learn(const char *action, const char *json)
{
	_autoblob_ struct blob_buf b = { 0 };

	CHECK(msg_parse(&b, json));
	db_learn(action, msg_root(&b));
}

static void test_db_cache(void)
{
	const struct cache_key patch = { .action = "patch", .scope = "12", .user = "Bob" };
	const struct cache_key patch_lower = { .action = "patch", .scope = "12", .user = "bob" };
	const struct cache_key patch_hc = { .action = "patch", .scope = "12", .user = "bob", .hardcore = true };
	const struct cache_key other = { .action = "patch", .scope = "99", .user = "bob" };
	const struct cache_key gameid = { .action = "gameid", .scope = "ABCDEF", .user = "" };
	const struct cache_key gameid_lower = { .action = "gameid", .scope = "abcdef", .user = "" };
	const struct cache_meta meta = { .form = "r=patch&u=bob&g=12&t=", .game = 12 };
	_autofree_ char *body = NULL, *hash_body = NULL;
	struct stale_ctx ctx = { 0 };
	size_t len = 0;

	CHECK(db_cache_put(&patch, "{\"Success\":true}\n2nd", 21, &meta) == 0);
	CHECK(db_cache_put(&patch_lower, "{\"Success\":true}\n2nd line", 26, &meta) == 0);
	CHECK(db_cache_put(&gameid, "{\"GameID\":12}", 13, NULL) == 0);
	body = db_cache_get(&patch, &len);
	CHECK(body && len == 26 && !memcmp(body, "{\"Success\":true}\n2nd line", 26));
	CHECK(db_cache_get(&patch_hc, NULL) == NULL);
	CHECK(db_cache_get(&other, NULL) == NULL);
	hash_body = db_cache_get(&gameid_lower, NULL);
	CHECK(hash_body && !strcmp(hash_body, "{\"GameID\":12}"));

	/* Not played yet: nothing to refresh. */
	db_cache_stale(0, INT64_MAX, stale_cb, &ctx);
	CHECK(ctx.n == 0);
	db_note_played(12);
	db_cache_stale(0, INT64_MAX, stale_cb, &ctx);
	CHECK(ctx.n == 1 && ctx.saw);
	/* Fetched after the staleness cutoff. */
	ctx.n = 0;
	db_cache_stale(0, 0, stale_cb, &ctx);
	CHECK(ctx.n == 0);
}

static void test_db_awards(void)
{
	_autoidset_ struct idset ids = { 0 }, hard = { 0 };
	struct db_award a = { .action = "awardachievement", .body = "r=awardachievement&a=5", .ua = "RA",
			      .user = "bob", .id = 5, .hardcore = true, .queued_at = 100 };
	struct db_award lb = { .action = "submitlbentry", .body = "r=submitlbentry&i=9", .ua = "RA",
			       .user = "bob", .id = 9 };
	_autofree_ struct db_award *next = NULL;

	learn("patch", "{\"Success\":true,\"PatchData\":{\"ID\":12,\"Achievements\":[{\"ID\":5},{\"ID\":6}]}}");
	learn("achievementsets", "{\"Success\":true,\"GameId\":40,\"Sets\":[{\"GameId\":40,\"Achievements\":"
				 "[{\"ID\":7}]},{\"GameId\":41,\"Achievements\":[{\"ID\":8},{\"ID\":101000001}]}]}");
	CHECK(db_game_for(5) == 12 && db_game_for(6) == 12);
	CHECK(db_game_for(7) == 40 && db_game_for(8) == 40);
	CHECK(db_game_for(101000001) == 0 && db_game_for(9) == 0);

	CHECK(db_award_count() == 0 && db_award_next() == NULL);
	CHECK(db_award_add(&a) == 0);
	CHECK(db_award_add(&lb) == 0);
	a.id = 6;
	a.hardcore = false;
	CHECK(db_award_add(&a) == 0);
	CHECK(db_award_count() == 3);
	CHECK(db_award_pending("awardachievement", 5, true, "BOB"));
	CHECK(!db_award_pending("awardachievement", 5, false, "bob"));

	db_award_pending_ids(12, "Bob", false, &ids);
	db_award_pending_ids(12, "bob", true, &hard);
	CHECK(ids.n == 2 && idset_has(&ids, 5) && idset_has(&ids, 6));
	CHECK(hard.n == 1 && idset_has(&hard, 5));

	next = db_award_next();
	CHECK(next && next->id == 5 && next->hardcore && next->queued_at == 100 && !strcmp(next->user, "bob") &&
	      !strcmp(next->body, "r=awardachievement&a=5"));
	db_award_retry(next->seq, "timeout");
	free(next);
	next = db_award_next();
	CHECK(next && next->id == 5 && next->retries == 1);
	db_award_delete(next->seq);
	free(next);
	next = db_award_next();
	CHECK(next && next->id == 9);
	db_award_fail(next->seq, "not found");
	CHECK(db_award_count() == 1);
	free(next);
	next = db_award_next();
	CHECK(next && next->id == 6);
}

static void test_msg(void)
{
	_autoblob_ struct blob_buf b = { 0 };
	_autoidset_ struct idset ids = { 0 };
	_autofree_ char *json = NULL;
	struct msg_table root;

	CHECK(!msg_parse(&b, "{\"a\":"));
	CHECK(!msg_parse(&b, "[1,2]"));
	CHECK(!msg_parse(&b, NULL));
	CHECK(msg_parse(&b, "{\"Success\":true,\"Big\":5000000000,\"Neg\":-5,\"S\":\"x\","
			   "\"Unlocks\":[{\"ID\":3,\"When\":1},{\"ID\":3},{\"ID\":0}],\"U\":[9,10,9]}"));
	root = msg_root(&b);
	CHECK(msg_success(root));
	CHECK(msg_int(root, "Big", 0) == 5000000000LL && msg_int(root, "Neg", 0) == -5);
	CHECK(msg_int(root, "S", 7) == 7 && !strcmp(msg_str(root, "S"), "x"));
	msg_collect_ids(msg_attr(root, "Unlocks", BLOBMSG_TYPE_ARRAY), &ids);
	msg_collect_ids(msg_attr(root, "U", BLOBMSG_TYPE_ARRAY), &ids);
	CHECK(ids.n == 3 && idset_has(&ids, 3) && idset_has(&ids, 9) && idset_has(&ids, 10));

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "Success", true);
	blobmsg_add_u64(&b, "Score", (uint64_t)-5);
	blobmsg_add_string(&b, "NumEntries", "1");
	json = msg_to_json(&b);
	CHECK_STR(xstrdup(json), "{\"Success\":true,\"Score\":-5,\"NumEntries\":\"1\"}");
}

int main(void)
{
	const char *tmp = getenv("TMPDIR");
	char dir[512], db_path[600], cmd[600];

	snprintf(dir, sizeof(dir), "%s/achd-unit-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(dir))
		return 2;
	snprintf(db_path, sizeof(db_path), "%s/test.db", dir);
	uloop_init();
	test_params();
	test_replay();
	test_msg();
	CHECK(db_open(db_path) == 0);
	test_db_cache();
	test_db_awards();
	db_close();
	/* Reopening an existing database keeps its contents. */
	CHECK(db_open(db_path) == 0);
	CHECK(db_award_count() == 1 && db_game_for(7) == 40);
	db_close();
	uloop_done();

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", dir);
	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	else
		printf("all unit checks passed\n");
	return failures ? 1 : 0;
}
