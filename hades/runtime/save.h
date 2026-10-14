#pragma once

typedef struct { int parent, work, lease, poisoned; } hades_save;
int hades_save_open(hades_save *save, int directory);
int hades_save_commit(hades_save *save);
int hades_save_close(hades_save *save);
