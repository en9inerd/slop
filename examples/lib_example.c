#include "slop.h"
#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: lib_example <file-or-dir>\n");
    return 1;
  }

  SlopOptions opts;
  slop_options_default(&opts);
  opts.include_general_smells = true;

  struct stat st;
  if (stat(argv[1], &st) != 0) {
    fprintf(stderr, "cannot stat %s\n", argv[1]);
    return 1;
  }

  if (S_ISDIR(st.st_mode)) {
    printf("=== Analyzing directory: %s ===\n\n", argv[1]);
    SlopProjectResult pr = slop_analyze_dir(argv[1], &opts);

    printf("Files: %d  (sloppy=%d, moderate=%d, clean=%d, skipped=%d)\n",
           pr.file_count, pr.sloppy, pr.moderate, pr.clean, pr.skipped);
    printf("Dup ratio: %.2f (%d/%d funcs)\n\n", pr.dup_ratio, pr.funcs_in_dup,
           pr.total_funcs);

    for (int i = 0; i < pr.file_count; i++) {
      SlopFileResult *f = &pr.files[i];
      if (f->skipped)
        continue;
      printf("  %-50s slop=%.1f  findings=%d  dead=%d\n", f->filepath,
             f->probability * 10.0, f->smells.count, f->dead_lines);
    }

    slop_project_result_free(&pr);
  } else {
    printf("=== Analyzing file: %s ===\n\n", argv[1]);
    SlopFileResult fr = slop_analyze_file(argv[1], &opts);

    if (fr.skipped) {
      printf("  skipped (binary/minified/generated)\n");
    } else {
      printf("  slop = %.1f / 10  (raw = %+.2f)\n", fr.probability * 10.0,
             fr.raw_score);
      printf("  lines: %d total, %d code, %d comment, %d blank\n",
             fr.total_lines, fr.code_lines, fr.comment_lines, fr.blank_lines);
      printf("  functions: %d  dead lines: %d\n", fr.function_count,
             fr.dead_lines);
      printf("  findings: %d\n", fr.smells.count);

      for (int i = 0; i < fr.smells.count; i++)
        printf("    line %-5d %s\n", fr.smells.items[i].line,
               fr.smells.items[i].message);
    }

    slop_file_result_free(&fr);
  }

  return 0;
}
