#include "main_panel.h"
#include "state.h"
#include "lvgl/lvgl.h"
#include "spdlog/spdlog.h"

#include <string>

LV_IMG_DECLARE(filament_img);
LV_IMG_DECLARE(light_img);
LV_IMG_DECLARE(move);
LV_IMG_DECLARE(print);
LV_IMG_DECLARE(extruder);
LV_IMG_DECLARE(bed);
LV_IMG_DECLARE(fan);
LV_IMG_DECLARE(heater);

LV_FONT_DECLARE(materialdesign_font_40);
#define MACROS_SYMBOL "\xF3\xB1\xB2\x83"
#define CONSOLE_SYMBOL "\xF3\xB0\x86\x8D"
#define TUNE_SYMBOL "\xF3\xB1\x95\x82"
#define HOME_SYMBOL "\xF3\xB0\x8B\x9C"
#define SETTING_SYMBOL "\xF3\xB0\x92\x93"


prompt_data *pdata;

MainPanel::MainPanel(KWebSocketClient &websocket,
		     std::mutex &lock,
		     SpoolmanPanel &sm)
  : NotifyConsumer(lock)
  , ws(websocket)
  , homing_panel(ws, lock)
  , fan_panel(ws, lock)
  , led_panel(ws, lock)    
  , tabview(lv_tabview_create(lv_scr_act(), LV_DIR_LEFT, 60))
  , main_tab(lv_tabview_add_tab(tabview, HOME_SYMBOL))
  , macros_tab(lv_tabview_add_tab(tabview, MACROS_SYMBOL))
  , macros_panel(ws, lock, macros_tab)
  , console_tab(lv_tabview_add_tab(tabview, CONSOLE_SYMBOL))
  , console_panel(ws, lock, console_tab)
  , printertune_tab(lv_tabview_add_tab(tabview, TUNE_SYMBOL))
  , setting_tab(lv_tabview_add_tab(tabview, SETTING_SYMBOL))
  , setting_panel(websocket, lock, setting_tab, sm)
  , main_cont(lv_obj_create(main_tab))
  , print_status_panel(websocket, lock, main_cont)
  , print_panel(ws, lock, print_status_panel)
  , printertune_panel(ws, lock, printertune_tab, print_status_panel.get_finetune_panel())
  , numpad(Numpad(main_cont))
  , extruder_panel(ws, lock, numpad, sm)
  , spoolman_panel(sm)
  , temp_cont(lv_obj_create(main_cont))
  , temp_chart(lv_chart_create(main_cont))
  , homing_btn(main_cont, &move, "Homing", &MainPanel::_handle_homing_cb, this)
  , extrude_btn(main_cont, &filament_img, "Extrude", &MainPanel::_handle_extrude_cb, this)
  , action_btn(main_cont, &fan, "Fans", &MainPanel::_handle_fanpanel_cb, this)
  , led_btn(main_cont, &light_img, "LED", &MainPanel::_handle_ledpanel_cb, this)
  , print_btn(main_cont, &print, "Print", &MainPanel::_handle_print_cb, this)
{
    lv_style_init(&style);
    lv_style_set_img_recolor_opa(&style, LV_OPA_30);
    lv_style_set_img_recolor(&style, lv_color_black());
    lv_style_set_border_width(&style, 0);
    lv_style_set_bg_color(&style, lv_palette_darken(LV_PALETTE_GREY, 4));

    ws.register_notify_update(this);    
    ws.register_method_callback("notify_gcode_response", "MainPanel",[this](json& d) { this->handle_macro_response(d); });

    pdata = (prompt_data *) calloc(1, sizeof(prompt_data));
    pdata->header = NULL;
    pdata->text = NULL;
    pdata->buttons = NULL;
}

MainPanel::~MainPanel() {
  if (tabview != NULL) {
    lv_obj_del(tabview);
    tabview = NULL;
  }

  free(pdata);

  sensors.clear();
}

void MainPanel::subscribe() {
  spdlog::trace("main panel subscribing");
  ws.send_jsonrpc("printer.gcode.help", [this](json &d) { console_panel.handle_macros(d); });
  print_panel.subscribe();
}

PrinterTunePanel& MainPanel::get_tune_panel() {
  return printertune_panel;
}

void MainPanel::init(json &j) {
  std::lock_guard<std::mutex> lock(lv_lock);
  for (const auto &el : sensors) {
    auto target_value = j[json::json_pointer(fmt::format("/result/status/{}/target", el.first))];
    if (!target_value.is_null()) {
      int target = target_value.template get<int>();
      el.second->update_target(target);
    }

    auto temp_value = j[json::json_pointer(fmt::format("/result/status/{}/temperature", el.first))];
    if (!temp_value.is_null()) {
      int value = temp_value.template get<int>();
      el.second->update_series(value);
      el.second->update_value(value);
    }
  }

  macros_panel.populate();

  auto fans = State::get_instance()->get_display_fans();
  print_status_panel.init(fans);
  printertune_panel.init(j);
}

void MainPanel::consume(json &j) {  
  std::lock_guard<std::mutex> lock(lv_lock);
  for (const auto &el : sensors) {
    auto target_value = j[json::json_pointer(fmt::format("/params/0/{}/target", el.first))];
    if (!target_value.is_null()) {
      int target = target_value.template get<int>();
      el.second->update_target(target);
    }

    auto temp_value = j[json::json_pointer(fmt::format("/params/0/{}/temperature", el.first))];
    if (!temp_value.is_null()) {
      int value = temp_value.template get<int>();
      el.second->update_series(value);
      el.second->update_value(value);
    }
  }  
}

static void scroll_begin_event(lv_event_t * e)
{
  /*Disable the scroll animations. Triggered when a tab button is clicked */
  if (lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN) {
    lv_anim_t * a = (lv_anim_t*)lv_event_get_param(e);
    if(a)  a->time = 0;
  }
}

void MainPanel::create_panel() {
  lv_obj_clear_flag(lv_tabview_get_content(tabview), LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(lv_tabview_get_content(tabview), scroll_begin_event, LV_EVENT_SCROLL_BEGIN, NULL);
  
  lv_obj_t * tab_btns = lv_tabview_get_tab_btns(tabview);
  lv_obj_set_style_bg_color(tab_btns, lv_palette_main(LV_PALETTE_GREY), LV_STATE_CHECKED | LV_PART_ITEMS);
  lv_obj_set_style_outline_width(tab_btns, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY | LV_STATE_FOCUS_KEY);
  lv_obj_set_style_border_side(tab_btns, 0, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_font(tab_btns, &materialdesign_font_40, LV_STATE_DEFAULT);

  // lv_obj_set_style_text_font(lv_scr_act(), LV_FONT_DEFAULT, 0);

  lv_obj_set_style_pad_all(main_tab, 0, 0);
  lv_obj_set_style_pad_all(macros_tab, 0, 0);
  lv_obj_set_style_pad_all(console_tab, 0, 0);
  lv_obj_set_style_pad_all(printertune_tab, 0, 0);
  lv_obj_set_style_pad_all(setting_tab, 0, 0);

  create_main(main_tab);
  
}

void MainPanel::handle_homing_cb(lv_event_t *event) {
  spdlog::trace("clicked homing1");
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    spdlog::trace("clicked homing");
    homing_panel.foreground();
  }
}

void MainPanel::handle_extrude_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    spdlog::trace("clicked extruder");
    extruder_panel.foreground();
  }
}

void MainPanel::handle_fanpanel_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    spdlog::trace("clicked fan panel");
    fan_panel.foreground();
  }
}

void MainPanel::handle_ledpanel_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    spdlog::trace("clicked led panel");
    led_panel.foreground();
  }
}

void MainPanel::handle_print_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    spdlog::trace("clicked print");
    print_panel.foreground();
  }
}

void MainPanel::create_main(lv_obj_t * parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW_WRAP);

    static lv_coord_t grid_main_row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t grid_main_col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
      LV_GRID_TEMPLATE_LAST};

    lv_obj_clear_flag(main_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(main_cont, LV_PCT(100));

    lv_obj_set_flex_grow(main_cont, 1);
    lv_obj_set_grid_dsc_array(main_cont, grid_main_col_dsc, grid_main_row_dsc);    
#ifdef GUPPY_FF5M
    lv_obj_set_style_pad_top(main_cont, 0, 0);
    lv_obj_set_style_pad_bottom(main_cont, 0, 0);
#endif


    lv_obj_set_grid_cell(homing_btn.get_container(), LV_GRID_ALIGN_CENTER, 2, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    lv_obj_set_grid_cell(extrude_btn.get_container(), LV_GRID_ALIGN_CENTER, 3, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    lv_obj_set_grid_cell(action_btn.get_container(), LV_GRID_ALIGN_CENTER, 2, 1, LV_GRID_ALIGN_CENTER, 1, 1);
    lv_obj_set_grid_cell(led_btn.get_container(), LV_GRID_ALIGN_CENTER, 3, 1, LV_GRID_ALIGN_CENTER, 1, 1);
    lv_obj_set_grid_cell(print_btn.get_container(), LV_GRID_ALIGN_CENTER, 2, 2, LV_GRID_ALIGN_CENTER, 2, 1);

    lv_obj_clear_flag(temp_cont, LV_OBJ_FLAG_SCROLLABLE);
#ifdef GUPPY_FF5M
    lv_obj_set_size(temp_cont, LV_PCT(50), LV_PCT(62));
    lv_obj_set_style_pad_top(temp_cont, 4, 0);
    lv_obj_set_style_pad_bottom(temp_cont, 0, 0);
#else
    lv_obj_set_size(temp_cont, LV_PCT(50), LV_PCT(50));
#endif
    
    lv_obj_set_style_pad_all(temp_cont, 0, 0);

    lv_obj_set_flex_flow(temp_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_grid_cell(temp_cont, LV_GRID_ALIGN_START, 0, 2, LV_GRID_ALIGN_START, 0, 2);
    
    lv_obj_align(temp_chart, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_size(temp_chart, LV_PCT(45), LV_PCT(40));
#ifdef GUPPY_FF5M
    lv_obj_set_style_pad_top(temp_chart, 0, 0);
    lv_obj_set_style_pad_bottom(temp_chart, 4, 0);
#endif
    lv_obj_set_style_size(temp_chart, 0, LV_PART_INDICATOR);

    lv_chart_set_range(temp_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 300);
    lv_obj_set_grid_cell(temp_chart, LV_GRID_ALIGN_END, 0, 2, LV_GRID_ALIGN_END, 2, 1);
    lv_chart_set_axis_tick(temp_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 0, 6, 5, true, 50);

    lv_chart_set_div_line_count(temp_chart, 3, 8);
    lv_chart_set_point_count(temp_chart, 5000);
    lv_chart_set_zoom_x(temp_chart, 5000);
    lv_obj_scroll_to_x(temp_chart, LV_COORD_MAX, LV_ANIM_OFF);
}

void MainPanel::create_sensors(json &temp_sensors) {
  std::lock_guard<std::mutex> lock(lv_lock);
  sensors.clear();
  for (auto &sensor : temp_sensors.items()) {
    std::string key = sensor.key();
    bool controllable = sensor.value()["controllable"].template get<bool>();

    lv_color_t color_code = lv_palette_main(LV_PALETTE_ORANGE);
    if (!sensor.value()["color"].is_number()) {
      std::string color = sensor.value()["color"].template get<std::string>();
      if (color == "red") {
	color_code = lv_palette_main(LV_PALETTE_RED);
      } else if (color == "purple") {
	color_code = lv_palette_main(LV_PALETTE_PURPLE);
      } else if (color == "blue") {
	color_code = lv_palette_main(LV_PALETTE_BLUE);	
      }
    } else {
      color_code = lv_palette_main((lv_palette_t)sensor.value()["color"].template get<int>());
    }

    std::string display_name = sensor.value()["display_name"].template get<std::string>();

    const void* sensor_img = &heater;
    if (key == "extruder") {
      sensor_img = &extruder;
    } else if (key == "heater_bed") {
      sensor_img = &bed;
    }

    lv_chart_series_t *temp_series =
      lv_chart_add_series(temp_chart, color_code, LV_CHART_AXIS_PRIMARY_Y);

    sensors.insert({key, std::make_shared<SensorContainer>(ws, temp_cont, sensor_img, 150,
			   display_name.c_str(), color_code, controllable, false, numpad, key,
        		   temp_chart, temp_series)});
  }
}

void MainPanel::create_fans(json &fans) {
  fan_panel.create_fans(fans);
}

void MainPanel::create_leds(json &leds) {
  led_panel.init(leds);
}

void MainPanel::enable_spoolman() {
  spoolman_panel.init();
  setting_panel.enable_spoolman();
  extruder_panel.enable_spoolman();
}

static void pdata_clean() {
    if (pdata->header) free(pdata->header);
    if (pdata->text) free(pdata->text);
    for (int i = 0; i < pdata->button_size; i++) { free(pdata->buttons[i].text); free(pdata->buttons[i].command); }
    if (pdata->buttons) free(pdata->buttons);
    pdata->header = NULL;
    pdata->text = NULL;
    pdata->buttons = NULL;
    pdata->button_size = 0;
}

void MainPanel::handle_macro_response(json &j) {
    spdlog::trace("macro response: {}", j.dump());
    auto &v = j["/params/0"_json_pointer];

    if (!v.is_null()) {
        spdlog::debug("data found");
        std::string resp = v.template get<std::string>();
        std::lock_guard<std::mutex> lock(lv_lock);
        spdlog::debug("data: {}", resp);

        if (resp.find("// action:", 0) == 0) {
            // it is an action
            std::string command = resp.substr(10);
            spdlog::debug("action: {}", command);
            if (command.find("prompt_begin") == 0) {
                pdata_clean();
                std::string prompt_header = command.substr(13);
                spdlog::debug("PROMPT_BEGIN: {}", prompt_header);

                if (pdata->header) free(pdata->header);
                pdata->header = (char *) calloc(sizeof(char), prompt_header.size() + 1);
                memcpy(pdata->header, prompt_header.c_str(), prompt_header.size());
            } else if (command.find("prompt_text") == 0) {
                std::string prompt_text = command.substr(12);
                spdlog::debug("PROMPT_TEXT: {}", prompt_text);
                if (pdata->text) free(pdata->text);
                pdata->text = (char *) calloc(sizeof(char), prompt_text.size() + 1);
                memcpy(pdata->text, prompt_text.c_str(), prompt_text.size());
            } else if (command.find("prompt_footer_button") == 0) {
                int index_first = command.find("|", 21);
                int index_second = command.find("|", index_first + 1);
                spdlog::debug("{} {}", index_first, index_second);
                std::string prompt_footer_button = command.substr(21, index_first - 21);
                std::string prompt_button_command;
                std::string prompt_button_type = "none";
                if (index_second > 0) {
                    prompt_button_command = command.substr(index_first + 1, index_second - index_first - 1);
                    prompt_button_type = command.substr(index_second + 1, command.length() - index_second - 1);
                } else {
                    prompt_button_command = command.substr(index_first + 1);
                }
                spdlog::debug("PROMPT_FOOTER_BUTTON: {} CMD: {}, type {}", prompt_footer_button, prompt_button_command, prompt_button_type);
                pdata->button_size++;
                pdata->buttons = (prompt_button *) realloc(pdata->buttons, sizeof(prompt_button) * pdata->button_size);
                prompt_button *btn = &pdata->buttons[pdata->button_size - 1];
                btn->text = (char *) calloc(sizeof(char), prompt_footer_button.size() + 1);
                memcpy(btn->text, prompt_footer_button.c_str(), prompt_footer_button.size());
                btn->command = (char *) calloc(sizeof(char), prompt_button_command.size() + 1);
                memcpy(btn->command, prompt_button_command.c_str(), prompt_button_command.size());
                if (prompt_button_type.compare("primary")) {
                    btn->type = 0;
                } else if (prompt_button_type.compare("secondary")) {
                    btn->type = 1;
                } else if (prompt_button_type.compare("info")) {
                    btn->type = 2;
                } else if (prompt_button_type.compare("warning")) {
                    btn->type = 3;
                } else if (prompt_button_type.compare("error")) {
                    btn->type = 4;
                } else {
                    btn->type = 0;
                }
                // types do nothing, cannot alter individual buttons
            } else if (command.find("prompt_show") == 0) {
                spdlog::debug("PROMPT_SHOW");
                lv_obj_t *msgbox = prompt(pdata);
                spdlog::debug("callback");
                lv_obj_add_event_cb(msgbox, [](lv_event_t *e) {
                    MainPanel *p = (MainPanel*)e->user_data;
                    lv_obj_t *obj = lv_obj_get_parent(lv_event_get_target(e));
                    uint32_t clicked_btn = lv_msgbox_get_active_btn(obj);
                    spdlog::debug("Button {} pressed", clicked_btn);
                    spdlog::debug("Command issued: {}", pdata->buttons[clicked_btn].command);
                    p->ws.gcode_script(fmt::format("{}", pdata->buttons[clicked_btn].command));
                    lv_msgbox_close(obj);

                }, LV_EVENT_VALUE_CHANGED, this);
            } else if (command.find("prompt_end") == 0) {
                spdlog::debug("PROMPT_END");
                pdata_clean(); 
            } else {
                spdlog::debug("action {} --- not supported", command);
            }
        }
        
    }
}

lv_obj_t *MainPanel::prompt(prompt_data *data) {

    static char **btns = (char **) calloc(data->button_size, sizeof(char *));
    for (int i = 0; i < data->button_size; i++) btns[i] = data->buttons[i].text;

    lv_obj_t *mbox1 = lv_msgbox_create(NULL, data->header, data->text, (const char **) btns, false);
    lv_obj_t *msg = ((lv_msgbox_t*)mbox1)->text;
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);  
    lv_obj_set_width(msg, LV_PCT(100));
    lv_obj_center(msg);

    lv_obj_t *btnm = lv_msgbox_get_btns(mbox1);
    for (int i = 0; i < data->button_size; i++) {
        lv_btnmatrix_set_btn_ctrl(btnm, i, LV_BTNMATRIX_CTRL_CHECKED);
    }
    lv_obj_add_flag(btnm, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(btnm, LV_ALIGN_BOTTOM_MID, 0, 0);

    auto hscale = (double)lv_disp_get_physical_ver_res(NULL) / 480.0;

    lv_obj_set_size(btnm, LV_PCT(90), (100 / data->button_size) * hscale);
    lv_obj_set_size(mbox1, LV_PCT(70), LV_PCT(50));
    lv_obj_center(mbox1);
    return mbox1;
}
