#include <gtk/gtk.h>

#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    const bool folder = argc == 2 && std::strcmp(argv[1], "--directory") == 0;
    if (!gtk_init_check(nullptr, nullptr))
        return 1;
    auto dialog = gtk_file_chooser_dialog_new(folder ? "Select Folder" : "Select File", nullptr,
        folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, folder ? "_Select" : "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    int status = 1;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        if (auto path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog))) {
            status = std::fputs(path, stdout) < 0 ? 1 : 0;
            g_free(path);
        }
    }
    gtk_widget_destroy(dialog);
    return status;
}
