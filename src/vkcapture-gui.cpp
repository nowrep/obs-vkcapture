// Copyright (C) 2026 Anthony Mendez <anthonymendez9@gmail.com>
//
// Use of this source code is governed by a GPL-2.0-or-later license that can
// be found in the LICENSE file.

#include "vkcapture-gui.h"

#include <QButtonGroup>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QTextStream>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs-module.h>

namespace {

// Represents a node in the Valve Data Format (VDF) tree structure.
struct VdfNode {
  QString key;
  QString value;
  bool is_object = false;
  QList<QSharedPointer<VdfNode>> children;

  QSharedPointer<VdfNode> FindChild(const QString &child_key) const {
    for (const auto &child : children) {
      if (child->key.compare(child_key, Qt::CaseInsensitive) == 0) {
        return child;
      }
    }
    return nullptr;
  }

  QSharedPointer<VdfNode> GetOrCreateChildObject(const QString &child_key) {
    auto child = FindChild(child_key);
    if (!child) {
      child = QSharedPointer<VdfNode>::create();
      child->key = child_key;
      child->is_object = true;
      children.append(child);
    } else {
      child->is_object = true;
    }
    return child;
  }

  void SetChildValue(const QString &child_key, const QString &child_val) {
    auto child = FindChild(child_key);
    if (!child) {
      child = QSharedPointer<VdfNode>::create();
      child->key = child_key;
      children.append(child);
    }
    child->is_object = false;
    child->value = child_val;
    child->children.clear();
  }
};

// Parses a VDF file from the given filepath into a tree representation.
QSharedPointer<VdfNode> ParseVdf(const QString &filepath) {
  QFile file(filepath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return nullptr;
  }
  QTextStream in(&file);
  QList<QString> all_tokens;
  while (!in.atEnd()) {
    QString line = in.readLine();
    int i = 0;
    while (i < line.length()) {
      if (line[i].isSpace()) {
        i++;
        continue;
      }
      if (line[i] == '/' && i + 1 < line.length() && line[i + 1] == '/') {
        break; // Comment
      }
      if (line[i] == '{') {
        all_tokens.append("{");
        i++;
      } else if (line[i] == '}') {
        all_tokens.append("}");
        i++;
      } else if (line[i] == '"') {
        i++; // skip open quote
        QString result;
        while (i < line.length()) {
          if (line[i] == '\\' && i + 1 < line.length()) {
            if (line[i + 1] == '"') {
              result.append('"');
              i += 2;
            } else if (line[i + 1] == '\\') {
              result.append('\\');
              i += 2;
            } else {
              result.append(line[i]);
              i++;
            }
          } else if (line[i] == '"') {
            i++;
            break;
          } else {
            result.append(line[i]);
            i++;
          }
        }
        all_tokens.append(result);
      } else {
        int start = i;
        while (i < line.length() && !line[i].isSpace() && line[i] != '{' &&
               line[i] != '}') {
          i++;
        }
        all_tokens.append(line.mid(start, i - start));
      }
    }
  }
  file.close();

  auto root = QSharedPointer<VdfNode>::create();
  root->is_object = true;
  QList<QSharedPointer<VdfNode>> stack;
  stack.append(root);

  int token_idx = 0;
  while (token_idx < all_tokens.size()) {
    QString tok = all_tokens[token_idx];
    if (tok == "{") {
      if (stack.last()->children.isEmpty()) {
        token_idx++;
        continue;
      }
      auto last_child = stack.last()->children.last();
      last_child->is_object = true;
      stack.append(last_child);
      token_idx++;
    } else if (tok == "}") {
      if (stack.size() > 1) {
        stack.removeLast();
      }
      token_idx++;
    } else {
      bool has_value = (token_idx + 1 < all_tokens.size()) &&
                       (all_tokens[token_idx + 1] != "{") &&
                       (all_tokens[token_idx + 1] != "}");
      auto node = QSharedPointer<VdfNode>::create();
      node->key = tok;
      if (has_value) {
        node->value = all_tokens[token_idx + 1];
        node->is_object = false;
        token_idx += 2;
      } else {
        node->is_object = true;
        token_idx += 1;
      }
      stack.last()->children.append(node);
    }
  }

  return root;
}

// Writes a single VdfNode and its children recursively to a QTextStream.
void WriteVdfNode(QTextStream &out, const QSharedPointer<VdfNode> &node,
                  int depth) {
  QString indent(depth, '\t');
  if (node->key.isEmpty()) {
    for (const auto &child : node->children) {
      WriteVdfNode(out, child, depth);
    }
    return;
  }

  auto escape_str = [](const QString &s) {
    QString res;
    for (int j = 0; j < s.length(); j++) {
      if (s[j] == '"') {
        res.append("\\\"");
      } else if (s[j] == '\\') {
        res.append("\\\\");
      } else {
        res.append(s[j]);
      }
    }
    return res;
  };

  if (node->is_object) {
    out << indent << "\"" << escape_str(node->key) << "\"\n";
    out << indent << "{\n";
    for (const auto &child : node->children) {
      WriteVdfNode(out, child, depth + 1);
    }
    out << indent << "}\n";
  } else {
    out << indent << "\"" << escape_str(node->key) << "\"\t\t\""
        << escape_str(node->value) << "\"\n";
  }
}

// Serializes the VDF tree structure back to the specified file path.
bool WriteVdf(const QSharedPointer<VdfNode> &root, const QString &filepath) {
  QFile file(filepath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    return false;
  }
  QTextStream out(&file);
  WriteVdfNode(out, root, 0);
  file.close();
  return true;
}

// Auto-detects the Steam installation directory, supporting standard and
// Flatpak paths.
QString GetSteamPath() {
  QString home = QDir::homePath();
  QStringList paths = {
      home + "/.steam/steam", home + "/.local/share/Steam",
      home + "/.var/app/com.valvesoftware.Steam/.steam/steam",
      home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam"};
  for (const auto &path : paths) {
    if (QDir(path).exists()) {
      return path;
    }
  }
  return "";
}

// Reads Steam's libraryfolders.vdf to find all Steam library paths.
QStringList GetLibraryFolders(const QString &steam_path) {
  QStringList folders;
  QString vdf_path = steam_path + "/steamapps/libraryfolders.vdf";
  auto root = ParseVdf(vdf_path);
  if (!root) {
    folders.append(steam_path);
    return folders;
  }
  auto lib_node = root->FindChild("libraryfolders");
  if (lib_node) {
    for (const auto &child : lib_node->children) {
      if (child->is_object) {
        auto path_node = child->FindChild("path");
        if (path_node && !path_node->value.isEmpty()) {
          folders.append(path_node->value);
        }
      }
    }
  }
  if (folders.isEmpty()) {
    folders.append(steam_path);
  }
  return folders;
}

// Scans appmanifest_*.acf files in the libraries to fetch all installed game
// AppIDs and names.
QMap<QString, QString> GetInstalledGames(const QStringList &library_folders) {
  QMap<QString, QString> games;
  for (const auto &folder : library_folders) {
    QDir dir(folder + "/steamapps");
    if (!dir.exists())
      continue;
    QStringList filters;
    filters << "appmanifest_*.acf";
    QStringList files = dir.entryList(filters, QDir::Files);
    for (const auto &filename : files) {
      QString file_path = dir.absoluteFilePath(filename);
      auto acf_root = ParseVdf(file_path);
      if (!acf_root)
        continue;
      auto app_state = acf_root->FindChild("AppState");
      if (app_state) {
        auto appid_node = app_state->FindChild("appid");
        auto name_node = app_state->FindChild("name");
        if (appid_node && name_node && !appid_node->value.isEmpty()) {
          games.insert(appid_node->value, name_node->value);
        }
      }
    }
  }
  return games;
}

// Modifies the LaunchOptions settings in the user's localconfig.vdf file.
bool ApplyLaunchOptions(const QString &localconfig_path,
                        const QMap<QString, QString> &games, int action_type) {
  auto root = ParseVdf(localconfig_path);
  if (!root)
    return false;

  auto user_store = root->GetOrCreateChildObject("UserLocalConfigStore");
  auto software = user_store->GetOrCreateChildObject("Software");
  auto valve = software->GetOrCreateChildObject("Valve");
  auto steam = valve->GetOrCreateChildObject("Steam");
  auto apps = steam->GetOrCreateChildObject("Apps");

  auto modify_opts = [](const QString &current_opts, const QString &game_name,
                        int action) -> QString {
    QString clean = current_opts.trimmed();
    QRegularExpression regex(
        "obs-gamecapture(?:\\s+OBS_VKCAPTURE_NAME=(?:\"[^\"]*\"|[^\\s]*))?\\s*",
        QRegularExpression::CaseInsensitiveOption);
    clean.replace(regex, "");
    clean = clean.trimmed();

    if (action == 2) { // Apply with game name
      QString escaped_name = game_name;
      escaped_name.replace("\"", "\\\"");
      QString wrapper =
          "obs-gamecapture OBS_VKCAPTURE_NAME=\"" + escaped_name + "\"";
      if (clean.isEmpty()) {
        clean = wrapper + " %command%";
      } else {
        if (clean.contains("%command%")) {
          clean.prepend(wrapper + " ");
        } else {
          clean = wrapper + " " + clean + " %command%";
        }
      }
    } else if (action == 1) { // Apply obs-gamecapture (no name)
      QString wrapper = "obs-gamecapture";
      if (clean.isEmpty()) {
        clean = wrapper + " %command%";
      } else {
        if (clean.contains("%command%")) {
          clean.prepend(wrapper + " ");
        } else {
          clean = wrapper + " " + clean + " %command%";
        }
      }
    } else if (action == 0) { // Remove
      if (clean == "%command%") {
        clean = "";
      }
    }

    return clean.simplified();
  };

  for (auto it = games.begin(); it != games.end(); ++it) {
    QString appid = it.key();
    QString game_name = it.value();

    auto app_node = apps->GetOrCreateChildObject(appid);
    auto launch_node = app_node->FindChild("LaunchOptions");
    QString current_opts = launch_node ? launch_node->value : "";

    QString new_opts = modify_opts(current_opts, game_name, action_type);

    if (new_opts.isEmpty()) {
      if (launch_node) {
        app_node->children.removeOne(launch_node);
      }
    } else {
      app_node->SetChildValue("LaunchOptions", new_opts);
    }
  }

  return WriteVdf(root, localconfig_path);
}

} // namespace

// Dialog window class for configuring and applying the launch options.
class SteamLaunchOptionsDialog : public QDialog {
  Q_OBJECT
public:
  explicit SteamLaunchOptionsDialog(QWidget *parent = nullptr)
      : QDialog(parent) {
    setWindowTitle(obs_module_text("SteamLaunchOptions.Title"));
    setMinimumWidth(500);

    auto *layout = new QVBoxLayout(this);

    auto *desc_label =
        new QLabel(obs_module_text("SteamLaunchOptions.Desc"), this);
    layout->addWidget(desc_label);

    group_ = new QButtonGroup(this);

    opt1_ = new QRadioButton(obs_module_text("SteamLaunchOptions.Opt1"), this);
    opt2_ = new QRadioButton(obs_module_text("SteamLaunchOptions.Opt2"), this);
    opt3_ = new QRadioButton(obs_module_text("SteamLaunchOptions.Opt3"), this);

    group_->addButton(opt1_, 1);
    group_->addButton(opt2_, 2);
    group_->addButton(opt3_, 0);

    layout->addWidget(opt1_);
    layout->addWidget(opt2_);
    layout->addWidget(opt3_);

    opt2_->setChecked(true);

    warning_label_ = new QLabel(this);
    warning_label_->setStyleSheet(
        "color: #ff3333; font-weight: bold; margin-top: 10px;");
    warning_label_->setWordWrap(true);
    layout->addWidget(warning_label_);

    UpdateSteamWarning();

    auto *btn_layout = new QHBoxLayout();
    apply_btn_ =
        new QPushButton(obs_module_text("SteamLaunchOptions.Apply"), this);
    auto *cancel_btn =
        new QPushButton(obs_module_text("SteamLaunchOptions.Cancel"), this);

    btn_layout->addStretch();
    btn_layout->addWidget(apply_btn_);
    btn_layout->addWidget(cancel_btn);
    layout->addLayout(btn_layout);

    connect(apply_btn_, &QPushButton::clicked, this,
            &SteamLaunchOptionsDialog::OnApply);
    connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);
  }

private:
  QButtonGroup *group_;
  QRadioButton *opt1_;
  QRadioButton *opt2_;
  QRadioButton *opt3_;
  QLabel *warning_label_;
  QPushButton *apply_btn_;

  bool IsSteamRunning() {
    QDir proc_dir("/proc");
    if (!proc_dir.exists()) {
      return false;
    }

    const QFileInfoList entries =
        proc_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
      bool is_pid = false;
      entry.fileName().toInt(&is_pid);
      if (!is_pid) {
        continue;
      }

      QFile comm_file(entry.absoluteFilePath() + "/comm");
      if (comm_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&comm_file);
        QString comm = in.readLine().trimmed();
        if (comm == "steam") {
          return true;
        }
      }
    }
    return false;
  }

  void UpdateSteamWarning() {
    if (IsSteamRunning()) {
      warning_label_->setText(
          obs_module_text("SteamLaunchOptions.WarnSteamRunning"));
      warning_label_->show();
    } else {
      warning_label_->hide();
    }
  }

  void OnApply() {
    QString steam_path = GetSteamPath();
    if (steam_path.isEmpty()) {
      QMessageBox::critical(this, windowTitle(),
                            "Steam installation path not found.");
      return;
    }

    QStringList libs = GetLibraryFolders(steam_path);
    QMap<QString, QString> games = GetInstalledGames(libs);
    if (games.isEmpty()) {
      QMessageBox::warning(
          this, windowTitle(),
          "No installed Steam games found in library folders.");
      return;
    }

    QDir userdata_dir(steam_path + "/userdata");
    if (!userdata_dir.exists()) {
      QMessageBox::critical(this, windowTitle(),
                            "Steam userdata folder not found.");
      return;
    }

    QStringList user_dirs =
        userdata_dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    int success_count = 0;
    int action_type = group_->checkedId();

    for (const auto &user_id : user_dirs) {
      bool ok;
      user_id.toInt(&ok);
      if (!ok)
        continue;

      QString localconfig_path =
          userdata_dir.absoluteFilePath(user_id + "/config/localconfig.vdf");
      if (QFile::exists(localconfig_path)) {
        if (ApplyLaunchOptions(localconfig_path, games, action_type)) {
          success_count++;
        }
      }
    }

    if (success_count > 0) {
      QString msg = obs_module_text("SteamLaunchOptions.Success");
      msg = msg.arg(games.size());
      QMessageBox::information(this, windowTitle(), msg);
      accept();
    } else {
      QMessageBox::critical(this, windowTitle(),
                            obs_module_text("SteamLaunchOptions.Error"));
    }
  }
};

extern "C" void show_steam_launch_options_dialog(void *parent) {
  SteamLaunchOptionsDialog dlg(static_cast<QWidget *>(parent));
  dlg.exec();
}

#include "vkcapture-gui.moc"
