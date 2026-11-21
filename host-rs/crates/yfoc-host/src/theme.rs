//! 元驭红黑视觉：配色、中文字体、YH 标志与几个自绘控件。

use std::sync::Arc;

use eframe::egui::{
    self, Color32, CornerRadius, FontData, FontDefinitions, FontFamily, FontId, Frame, Margin, Mesh, Pos2, Rect,
    Response, RichText, Sense, Shadow, Stroke, StrokeKind, TextStyle, TextureHandle, TextureOptions, Ui, Vec2,
};

/** @brief 最深的底色，用于面板背景。 */
pub const BG: Color32 = Color32::from_rgb(0x0F, 0x0F, 0x11);
/** @brief 石墨黑 #171719，顶栏与浮层。 */
pub const PANEL: Color32 = Color32::from_rgb(0x17, 0x17, 0x19);
/** @brief 卡片底色。 */
pub const CARD: Color32 = Color32::from_rgb(0x1C, 0x1C, 0x1F);
/** @brief 选中或悬停的卡片底色。 */
pub const CARD_HI: Color32 = Color32::from_rgb(0x24, 0x24, 0x28);
/** @brief 输入框、按钮底色。 */
pub const FIELD: Color32 = Color32::from_rgb(0x26, 0x26, 0x2A);
/** @brief 细边框。 */
pub const STROKE: Color32 = Color32::from_rgb(0x2E, 0x2E, 0x33);
/** @brief 暖白 #FAFAF7。 */
pub const TEXT: Color32 = Color32::from_rgb(0xEC, 0xEC, 0xE8);
/** @brief 次要文字。 */
pub const MUTED: Color32 = Color32::from_rgb(0x8A, 0x8A, 0x92);
/** @brief 元驭红 #C51E2F。 */
pub const RED: Color32 = Color32::from_rgb(0xC5, 0x1E, 0x2F);
/** @brief 悬停时略亮的红。 */
pub const RED_HI: Color32 = Color32::from_rgb(0xE0, 0x3A, 0x4B);
/** @brief 按下时的暗红。 */
pub const RED_DIM: Color32 = Color32::from_rgb(0x8E, 0x15, 0x22);
/** @brief 点睛黄 #F2C94C，只做少量状态提示。 */
pub const YELLOW: Color32 = Color32::from_rgb(0xF2, 0xC9, 0x4C);
/** @brief 运行、在线。 */
pub const GREEN: Color32 = Color32::from_rgb(0x3D, 0xD6, 0x8C);

/** @brief 粗体字族名，找不到粗体文件时回落到常规字体。 */
pub const BOLD: &str = "bold";

const MARK_PNG: &[u8] = include_bytes!("../assets/mark.png");

/**
 * @brief 一次性装好深色主题、字体与间距。
 *
 * eframe 默认跟随系统明暗，这里强制深色，避免浅色系统下界面发白。
 */
pub fn install(ctx: &egui::Context) {
    install_fonts(ctx);
    ctx.set_theme(egui::ThemePreference::Dark);
    ctx.set_visuals_of(egui::Theme::Dark, visuals());
    ctx.set_style_of(egui::Theme::Dark, style(ctx));
}

fn visuals() -> egui::Visuals {
    let mut v = egui::Visuals::dark();
    let radius = CornerRadius::same(6);
    v.override_text_color = None;
    v.panel_fill = BG;
    v.window_fill = PANEL;
    v.extreme_bg_color = Color32::from_rgb(0x0B, 0x0B, 0x0D);
    v.faint_bg_color = CARD;
    v.code_bg_color = FIELD;
    v.hyperlink_color = YELLOW;
    v.warn_fg_color = YELLOW;
    v.error_fg_color = RED_HI;
    v.window_stroke = Stroke::new(1.0_f32, STROKE);
    v.window_corner_radius = CornerRadius::same(10);
    v.menu_corner_radius = CornerRadius::same(8);
    v.window_shadow = Shadow { offset: [0, 10], blur: 28, spread: 0, color: Color32::from_black_alpha(140) };
    v.popup_shadow = Shadow { offset: [0, 6], blur: 16, spread: 0, color: Color32::from_black_alpha(120) };
    v.selection.bg_fill = Color32::from_rgba_unmultiplied(0xC5, 0x1E, 0x2F, 150);
    v.selection.stroke = Stroke::new(1.0_f32, TEXT);
    v.slider_trailing_fill = true;
    v.striped = false;

    let w = &mut v.widgets;
    w.noninteractive.bg_fill = CARD;
    w.noninteractive.weak_bg_fill = CARD;
    w.noninteractive.bg_stroke = Stroke::new(1.0_f32, STROKE);
    w.noninteractive.fg_stroke = Stroke::new(1.0_f32, TEXT);
    w.noninteractive.corner_radius = radius;

    w.inactive.bg_fill = FIELD;
    w.inactive.weak_bg_fill = FIELD;
    w.inactive.bg_stroke = Stroke::new(1.0_f32, STROKE);
    w.inactive.fg_stroke = Stroke::new(1.0_f32, Color32::from_rgb(0xD8, 0xD8, 0xD4));
    w.inactive.corner_radius = radius;

    w.hovered.bg_fill = Color32::from_rgb(0x31, 0x31, 0x37);
    w.hovered.weak_bg_fill = Color32::from_rgb(0x31, 0x31, 0x37);
    w.hovered.bg_stroke = Stroke::new(1.0_f32, Color32::from_rgb(0x5A, 0x2A, 0x30));
    w.hovered.fg_stroke = Stroke::new(1.5_f32, TEXT);
    w.hovered.corner_radius = radius;
    w.hovered.expansion = 0.0;

    w.active.bg_fill = RED_DIM;
    w.active.weak_bg_fill = RED_DIM;
    w.active.bg_stroke = Stroke::new(1.0_f32, RED);
    w.active.fg_stroke = Stroke::new(1.5_f32, Color32::WHITE);
    w.active.corner_radius = radius;
    w.active.expansion = 0.0;

    w.open.bg_fill = FIELD;
    w.open.weak_bg_fill = FIELD;
    w.open.bg_stroke = Stroke::new(1.0_f32, RED);
    w.open.fg_stroke = Stroke::new(1.0_f32, TEXT);
    w.open.corner_radius = radius;
    v
}

fn style(ctx: &egui::Context) -> egui::Style {
    let mut style = (*ctx.style_of(egui::Theme::Dark)).clone();
    style.visuals = visuals();
    style.spacing.item_spacing = egui::vec2(8.0, 8.0);
    style.spacing.button_padding = egui::vec2(14.0, 6.0);
    style.spacing.interact_size = egui::vec2(40.0, 28.0);
    style.spacing.combo_width = 120.0;
    style.spacing.slider_width = 160.0;
    style.spacing.scroll.floating = true;
    style.spacing.scroll.bar_width = 6.0;
    style.text_styles = [
        (TextStyle::Heading, FontId::new(19.0, FontFamily::Proportional)),
        (TextStyle::Body, FontId::new(14.0, FontFamily::Proportional)),
        (TextStyle::Button, FontId::new(14.0, FontFamily::Proportional)),
        (TextStyle::Small, FontId::new(11.5, FontFamily::Proportional)),
        (TextStyle::Monospace, FontId::new(12.5, FontFamily::Monospace)),
    ]
    .into();
    style
}

/** @brief 系统字体候选：路径与 `.ttc` 内的字体序号。 */
struct FontCandidate {
    path: &'static str,
    index: u32,
}

const REGULAR: &[FontCandidate] = &[
    FontCandidate { path: "C:\\Windows\\Fonts\\msyh.ttc", index: 1 },
    FontCandidate { path: "C:\\Windows\\Fonts\\simhei.ttf", index: 0 },
    FontCandidate { path: "C:\\Windows\\Fonts\\simsun.ttc", index: 0 },
    FontCandidate { path: "/System/Library/Fonts/PingFang.ttc", index: 0 },
    FontCandidate { path: "/System/Library/Fonts/Hiragino Sans GB.ttc", index: 0 },
    FontCandidate { path: "/System/Library/Fonts/STHeiti Light.ttc", index: 0 },
    FontCandidate { path: "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", index: 2 },
    FontCandidate { path: "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", index: 2 },
    FontCandidate { path: "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", index: 2 },
    FontCandidate { path: "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", index: 0 },
    FontCandidate { path: "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc", index: 0 },
];

const BOLD_FILES: &[FontCandidate] = &[
    FontCandidate { path: "C:\\Windows\\Fonts\\msyhbd.ttc", index: 1 },
    FontCandidate { path: "/System/Library/Fonts/STHeiti Medium.ttc", index: 0 },
    FontCandidate { path: "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", index: 2 },
    FontCandidate { path: "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc", index: 2 },
];

fn read_first(candidates: &[FontCandidate]) -> Option<FontData> {
    candidates.iter().find_map(|item| {
        let bytes = std::fs::read(item.path).ok()?;
        let mut data = FontData::from_owned(bytes);
        data.index = item.index;
        Some(data)
    })
}

/**
 * @brief 运行时加载系统中文字体，避免打包十几 MB 的字体文件。
 *
 * 中文字体放在比例字族最前面，等宽字族放在 Hack 之后做回退。
 */
fn install_fonts(ctx: &egui::Context) {
    let mut fonts = FontDefinitions::default();
    let defaults = fonts.families.get(&FontFamily::Proportional).cloned().unwrap_or_default();
    let mut bold_family = Vec::new();
    if let Some(regular) = read_first(REGULAR) {
        fonts.font_data.insert("cjk".into(), Arc::new(regular));
        fonts.families.entry(FontFamily::Proportional).or_default().insert(0, "cjk".into());
        fonts.families.entry(FontFamily::Monospace).or_default().push("cjk".into());
        if let Some(bold) = read_first(BOLD_FILES) {
            fonts.font_data.insert("cjk-bold".into(), Arc::new(bold));
            bold_family.push("cjk-bold".to_string());
        }
        bold_family.push("cjk".into());
    }
    bold_family.extend(defaults);
    fonts.families.insert(FontFamily::Name(BOLD.into()), bold_family);
    ctx.set_fonts(fonts);
}

/** @brief 指定字号的粗体。 */
pub fn bold(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name(BOLD.into()))
}

/**
 * @brief 解码 YH 标志，把黑色字形换成暖白，地平线保持红黄渐变。
 *
 * 深色底使用暖白字形是品牌规范的要求。
 */
fn reverse_mark() -> image::RgbaImage {
    let mut img = image::load_from_memory_with_format(MARK_PNG, image::ImageFormat::Png)
        .expect("内置标志")
        .to_rgba8();
    for pixel in img.pixels_mut() {
        let [r, g, b, a] = pixel.0;
        let hi = r.max(g).max(b);
        let lo = r.min(g).min(b);
        if a > 0 && hi < 110 && hi - lo < 40 {
            pixel.0 = [0xFA, 0xFA, 0xF7, a];
        }
    }
    img
}

/** @brief 顶栏标志纹理，按 3 倍像素预缩放，避免 GPU 缩小时出现锯齿。 */
pub struct Brand {
    /** @brief 已上传的纹理。 */
    pub mark: TextureHandle,
    /** @brief 宽高比。 */
    pub aspect: f32,
}

impl Brand {
    /** @brief 上传纹理，只在启动时调用一次。 */
    pub fn load(ctx: &egui::Context) -> Self {
        let img = reverse_mark();
        let height = 96u32;
        let width = (img.width() as f32 * height as f32 / img.height() as f32).round() as u32;
        let small = image::imageops::resize(&img, width, height, image::imageops::FilterType::CatmullRom);
        let color = egui::ColorImage::from_rgba_unmultiplied([width as usize, height as usize], small.as_raw());
        Self {
            mark: ctx.load_texture("yh-mark", color, TextureOptions::LINEAR),
            aspect: width as f32 / height as f32,
        }
    }

    /** @brief 按高度绘制标志。 */
    pub fn show(&self, ui: &mut Ui, height: f32) -> Response {
        let size = Vec2::new(height * self.aspect, height);
        ui.add(egui::Image::new((self.mark.id(), size)))
    }
}

/** @brief 窗口图标：石墨黑圆角方块上放暖白 YH。 */
pub fn window_icon() -> egui::IconData {
    const SIZE: u32 = 256;
    const RADIUS: f32 = 56.0;
    let mark = reverse_mark();
    let mark_w = 208u32;
    let mark_h = (mark.height() as f32 * mark_w as f32 / mark.width() as f32).round() as u32;
    let mark = image::imageops::resize(&mark, mark_w, mark_h, image::imageops::FilterType::CatmullRom);
    let off_x = (SIZE - mark_w) / 2;
    let off_y = (SIZE - mark_h) / 2;
    let mut rgba = vec![0u8; (SIZE * SIZE * 4) as usize];
    for y in 0..SIZE {
        for x in 0..SIZE {
            let cx = (x as f32 + 0.5).clamp(RADIUS, SIZE as f32 - RADIUS);
            let cy = (y as f32 + 0.5).clamp(RADIUS, SIZE as f32 - RADIUS);
            let dist = ((x as f32 + 0.5 - cx).powi(2) + (y as f32 + 0.5 - cy).powi(2)).sqrt();
            let coverage = (RADIUS - dist + 0.5).clamp(0.0, 1.0);
            let mut px = [23.0_f32, 23.0, 25.0];
            if x >= off_x && x < off_x + mark_w && y >= off_y && y < off_y + mark_h {
                let src = mark.get_pixel(x - off_x, y - off_y).0;
                let alpha = src[3] as f32 / 255.0;
                for c in 0..3 {
                    px[c] = px[c] * (1.0 - alpha) + src[c] as f32 * alpha;
                }
            }
            let at = ((y * SIZE + x) * 4) as usize;
            rgba[at] = px[0] as u8;
            rgba[at + 1] = px[1] as u8;
            rgba[at + 2] = px[2] as u8;
            rgba[at + 3] = (coverage * 255.0) as u8;
        }
    }
    egui::IconData { rgba, width: SIZE, height: SIZE }
}

/**
 * @brief 地平线渐变条：左端淡入，主体元驭红，65%–95% 过渡到点睛黄。
 */
pub fn horizon(painter: &egui::Painter, rect: Rect) {
    let stops = [
        (0.0, Color32::from_rgba_unmultiplied(0xC5, 0x1E, 0x2F, 0)),
        (0.12, RED),
        (0.65, RED),
        (0.95, YELLOW),
        (1.0, Color32::from_rgba_unmultiplied(0xF2, 0xC9, 0x4C, 60)),
    ];
    let mut mesh = Mesh::default();
    for (index, (t, color)) in stops.iter().enumerate() {
        let x = rect.left() + rect.width() * t;
        mesh.colored_vertex(Pos2::new(x, rect.top()), *color);
        mesh.colored_vertex(Pos2::new(x, rect.bottom()), *color);
        if index > 0 {
            let base = (index as u32 - 1) * 2;
            mesh.add_triangle(base, base + 1, base + 2);
            mesh.add_triangle(base + 1, base + 2, base + 3);
        }
    }
    painter.add(mesh);
}

/** @brief 卡片容器。 */
pub fn card() -> Frame {
    Frame::new()
        .fill(CARD)
        .stroke(Stroke::new(1.0_f32, STROKE))
        .corner_radius(CornerRadius::same(10))
        .inner_margin(Margin::same(14))
}

/** @brief 带红色竖条的小节标题。 */
pub fn section(ui: &mut Ui, title: &str) {
    ui.horizontal(|ui| {
        let (rect, _) = ui.allocate_exact_size(Vec2::new(3.0, 15.0), Sense::hover());
        ui.painter().rect_filled(rect, CornerRadius::same(2), RED);
        ui.label(RichText::new(title).font(bold(14.5)).color(TEXT));
    });
}

/** @brief 实心红色主按钮，悬停时提亮。 */
pub fn primary(ui: &mut Ui, enabled: bool, text: &str) -> Response {
    let button = egui::Button::new(RichText::new(text).color(Color32::WHITE).font(bold(14.0)))
        .fill(RED)
        .stroke(Stroke::NONE);
    let response = ui.add_enabled(enabled, button);
    if enabled && response.hovered() {
        let fill = if response.is_pointer_button_down_on() { Color32::from_black_alpha(50) } else { Color32::from_white_alpha(18) };
        ui.painter().rect_filled(response.rect, CornerRadius::same(6), fill);
    }
    response
}

/** @brief 红色描边按钮，用于停止类操作。 */
pub fn danger(ui: &mut Ui, enabled: bool, text: &str) -> Response {
    let button = egui::Button::new(RichText::new(text).color(RED_HI))
        .fill(Color32::from_rgba_unmultiplied(0xC5, 0x1E, 0x2F, 22))
        .stroke(Stroke::new(1.0_f32, RED));
    ui.add_enabled(enabled, button)
}

/**
 * @brief 可切换的胶囊标签。`color` 为空时使用元驭红。
 *
 * 用于示波开关、通道勾选，比复选框更紧凑。
 */
pub fn chip(ui: &mut Ui, on: bool, text: &str, color: Option<Color32>) -> Response {
    let accent = color.unwrap_or(RED);
    let font = FontId::proportional(12.5);
    let galley = ui.painter().layout_no_wrap(text.to_owned(), font, if on { TEXT } else { MUTED });
    let dot = if color.is_some() { 14.0 } else { 0.0 };
    let size = Vec2::new(galley.size().x + 20.0 + dot, 26.0);
    let (rect, response) = ui.allocate_exact_size(size, Sense::click());
    let hovered = response.hovered();
    let fill = if on {
        Color32::from_rgba_unmultiplied(accent.r(), accent.g(), accent.b(), 38)
    } else if hovered {
        CARD_HI
    } else {
        FIELD
    };
    let stroke = if on { Stroke::new(1.0_f32, accent) } else { Stroke::new(1.0_f32, STROKE) };
    let painter = ui.painter();
    painter.rect(rect, CornerRadius::same(13), fill, stroke, StrokeKind::Inside);
    let mut x = rect.left() + 10.0;
    if color.is_some() {
        let tint = if on { accent } else { accent.gamma_multiply(0.45) };
        painter.circle_filled(Pos2::new(x + 4.0, rect.center().y), 4.0, tint);
        x += dot;
    }
    painter.galley(Pos2::new(x, rect.center().y - galley.size().y / 2.0), galley, TEXT);
    response.on_hover_cursor(egui::CursorIcon::PointingHand)
}

/** @brief 下划线式分页标签，返回新的选中下标。 */
pub fn tabs(ui: &mut Ui, current: usize, names: &[&str]) -> usize {
    let mut chosen = current;
    let row = ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = 4.0;
        for (index, name) in names.iter().enumerate() {
            let on = index == current;
            let font = if on { bold(14.5) } else { FontId::proportional(14.5) };
            let galley = ui.painter().layout_no_wrap((*name).to_owned(), font, if on { TEXT } else { MUTED });
            let size = Vec2::new(galley.size().x + 28.0, 36.0);
            let (rect, response) = ui.allocate_exact_size(size, Sense::click());
            let painter = ui.painter();
            if response.hovered() && !on {
                painter.rect_filled(rect.shrink2(Vec2::new(0.0, 4.0)), CornerRadius::same(6), CARD_HI);
            }
            let text_pos = Pos2::new(rect.center().x - galley.size().x / 2.0, rect.center().y - galley.size().y / 2.0 - 1.0);
            painter.galley(text_pos, galley, TEXT);
            if on {
                let bar = Rect::from_min_max(
                    Pos2::new(rect.left() + 10.0, rect.bottom() - 3.0),
                    Pos2::new(rect.right() - 10.0, rect.bottom() - 1.0),
                );
                horizon(painter, bar);
            }
            if response.on_hover_cursor(egui::CursorIcon::PointingHand).clicked() {
                chosen = index;
            }
        }
    });
    let y = row.response.rect.bottom() - 0.5;
    let full = ui.max_rect();
    ui.painter().line_segment([Pos2::new(full.left(), y), Pos2::new(full.right(), y)], Stroke::new(1.0_f32, STROKE));
    chosen
}

/** @brief 圆角徽标。 */
pub fn badge(ui: &mut Ui, text: &str, color: Color32) {
    Frame::new()
        .fill(Color32::from_rgba_unmultiplied(color.r(), color.g(), color.b(), 34))
        .corner_radius(CornerRadius::same(9))
        .inner_margin(Margin::symmetric(7, 1))
        .show(ui, |ui| {
            ui.label(RichText::new(text).size(11.0).color(color));
        });
}

/** @brief 状态小圆点。 */
pub fn dot(ui: &mut Ui, color: Color32) {
    let (rect, _) = ui.allocate_exact_size(Vec2::splat(10.0), Sense::hover());
    ui.painter().circle_filled(rect.center(), 6.0, Color32::from_rgba_unmultiplied(color.r(), color.g(), color.b(), 40));
    ui.painter().circle_filled(rect.center(), 4.0, color);
}

/** @brief 设备状态对应的颜色。 */
pub fn state_color(state: &str) -> Color32 {
    match state {
        "RUNNING" => GREEN,
        "READY" => YELLOW,
        "FAULT" => RED_HI,
        "CALIBRATING" => YELLOW,
        _ => MUTED,
    }
}
