# gamedb.c against assets/gamedb.txt, and its title keys against
# tools/make_gamedb.py's (keys.txt is written from the same file by Python).
V = ../..
test: gamedb_test keys.txt
	./gamedb_test $(V)/assets/gamedb.txt keys.txt
gamedb_test: gamedb_test.c $(V)/gamedb.c $(V)/gamedb.h
	$(CC) -O2 -Wall -I$(V) gamedb_test.c $(V)/gamedb.c -o $@
keys.txt: $(V)/assets/gamedb.txt $(V)/tools/make_gamedb.py
	python3 -c "import sys; sys.path.insert(0, '$(V)/tools'); import make_gamedb as m; \
	t = set(); \
	[t.update((v, m.base_title(v))) for k, v in (l.rstrip('\n').split('\t', 1) for l in open('$(V)/assets/gamedb.txt', encoding='utf-8')) if k[0] in 'CS']; \
	t |= {'Pokémon Émerald & Friends', 'The The', '  A -- B  ', 'Ça va', 'Æon Flux', 'Señor Ñandú', 'x×y'}; \
	open('keys.txt', 'w', encoding='utf-8').writelines('%s\t%s\n' % (s, m.title_key(s)) for s in sorted(t))"
clean:
	rm -f gamedb_test keys.txt
