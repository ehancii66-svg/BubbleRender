import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { common } from '@kit.AbilityKit';

const DOMAIN = 0x0000;

@Entry
@Component
struct Index {
    private xcomponentId: string = 'xcomponent'
    private xComponentController: XComponentController = new XComponentController()

    private renderTimerId: number = -1
    private fpsUpdateCounter: number = 0

    @State private compX: number = 0;
    @State private compY: number = 0;
    @State private compW: number = 0;
    @State private compH: number = 0;
    @State private showSettings: boolean = false;
    @State private fpsText: string = 'FPS: --  RS: 0.50';

    private lastTouchX: number = 0
    private lastTouchY: number = 0
    private touchMoved: boolean = false

    private startRenderLoop(): void {
        if (this.renderTimerId >= 0) {
            return
        }
        this.renderTimerId = setInterval(() => {
            try {
                testNapi.renderFrame()
                this.fpsUpdateCounter += 1
                if (this.fpsUpdateCounter >= 15) {
                    this.fpsUpdateCounter = 0
                    const fps = testNapi.getFps()
                    if (fps > 0) {
                        this.fpsText = `FPS: ${fps.toFixed(1)}  RS: 0.50`
                    }
                }
            } catch (err) {
                hilog.error(DOMAIN, 'Index', 'renderFrame failed: %{public}s', JSON.stringify(err))
            }
        }, 16)
        hilog.info(DOMAIN, 'Index', 'render loop started')
    }

    private stopRenderLoop(): void {
        if (this.renderTimerId < 0) {
            return
        }
        clearInterval(this.renderTimerId)
        this.renderTimerId = -1
        hilog.info(DOMAIN, 'Index', 'render loop stopped')
    }

    private mapAndDispatchTouch(event: TouchEvent): void {
        if (this.showSettings) return

        let points = event.touches
        if (!points || points.length === 0) {
            points = event.changedTouches
        }
        if (!points || points.length === 0) {
            return
        }

        const touch = points[0]
        let touchType = 2
        switch (event.type) {
            case TouchType.Down:
                touchType = 0
                this.lastTouchX = touch.x
                this.lastTouchY = touch.y
                this.touchMoved = false
                break
            case TouchType.Up:
                touchType = 1
                break
            case TouchType.Move:
                touchType = 2
                break
        }
        const hasArea = this.compW > 0 && this.compH > 0
        const localX = hasArea ? (touch.x - this.compX) : touch.x
        const localY = hasArea ? (touch.y - this.compY) : touch.y

        const inComp = hasArea ? (localX >= 0 && localX <= this.compW && localY >= 0 && localY <= this.compH) : true
        if (!inComp && touchType !== 1) {
            return
        }

        // ---- drag to rotate camera ----
        if (touchType === 2) {
            const dx = touch.x - this.lastTouchX
            const dy = touch.y - this.lastTouchY
            if (Math.abs(dx) > 1 || Math.abs(dy) > 1) {
                this.touchMoved = true
            }
            if (this.touchMoved) {
                try {
                    testNapi.rotateCamera(dx, dy)
                } catch (e) {
                    hilog.error(DOMAIN, 'Index', 'rotateCamera failed: %{public}s', JSON.stringify(e))
                }
                this.lastTouchX = touch.x
                this.lastTouchY = touch.y
                return
            }
        }

        hilog.info(
            DOMAIN,
            'Index',
            `onTouch map global=(${touch.x},${touch.y}) local=(${localX},${localY}) type=${touchType} inComp=${inComp}`
        )

        try {
            testNapi.onTouch(localX, localY, touchType)
        } catch (err) {
            hilog.error(DOMAIN, 'Index', 'call onTouch failed: %{public}s', JSON.stringify(err))
        }
    }

    aboutToAppear(): void {
        this.startRenderLoop()
    }

    aboutToDisappear(): void {
        this.stopRenderLoop()
        testNapi.releaseResource()
    }

    @Builder SettingSlider(label: string, defVal: number, minVal: number, maxVal: number,
                           stepVal: number, onChange: (val: number) => void) {
        Row() {
            Text(label).fontSize(10).fontColor(Color.White).width(60)
            Slider({ value: defVal, min: minVal, max: maxVal, step: stepVal, style: SliderStyle.OutSet })
                .width(140).height(18)
                .onChange((val: number) => { onChange(val) })
        }.width('100%').justifyContent(FlexAlign.Center).padding({ top: 2, bottom: 2 })
    }

    build() {
        Stack() {
            XComponent({
                id: this.xcomponentId,
                type: XComponentType.SURFACE,
                libraryname: 'entry',
                controller: this.xComponentController
            })
                .onLoad(() => {
                    const context = getContext(this) as common.UIAbilityContext
                    let fileDir = context.filesDir
                    hilog.info(0x0000, 'Test', `filesDir = ${fileDir}`)
                    const surfaceId = this.xComponentController.getXComponentSurfaceId()
                    testNapi.initGraphics(context.resourceManager, surfaceId, fileDir)
                    if (this.compW > 0 && this.compH > 0) {
                        testNapi.setInputViewport(this.compW, this.compH)
                    }
                    this.startRenderLoop()
                })
                .onAreaChange((_oldValue: Area, newValue: Area) => {
                    this.compX = Number(newValue.position.x)
                    this.compY = Number(newValue.position.y)
                    this.compW = Number(newValue.width)
                    this.compH = Number(newValue.height)
                    hilog.info(DOMAIN, 'Index', `XComponent area x=${this.compX}, y=${this.compY}, w=${this.compW}, h=${this.compH}`)
                    try {
                        testNapi.setInputViewport(this.compW, this.compH)
                    } catch (err) {
                        hilog.error(DOMAIN, 'Index', 'setInputViewport failed: %{public}s', JSON.stringify(err))
                    }
                })
                .width('100%')
                .height('100%')

            Text(this.fpsText)
                .fontSize(12)
                .fontColor(Color.White)
                .fontWeight(FontWeight.Medium)
                .padding({ left: 8, right: 8, top: 4, bottom: 4 })
                .backgroundColor('rgba(0,0,0,0.42)')
                .borderRadius(6)
                .position({ x: 8, y: 6 })

            // ---- Compact top bar ----
            Row() {
                Button('Kim2012').fontSize(10).width(60).height(28)
                    .onClick(() => { try { testNapi.setIridescenceMode(0) } catch (e) {} })
                Button('LUT').fontSize(10).width(48).height(28)
                    .onClick(() => { try { testNapi.setIridescenceMode(1) } catch (e) {} })
                Button('Airy').fontSize(10).width(48).height(28)
                    .onClick(() => { try { testNapi.setIridescenceMode(2) } catch (e) {} })
                Button('⏯').fontSize(10).width(40).height(28)
                    .onClick(() => { try { testNapi.toggleSimulation() } catch (e) {} })
                Button('⚙').fontSize(14).width(40).height(28)
                    .onClick(() => { this.showSettings = !this.showSettings })
            }
            .width('100%').justifyContent(FlexAlign.Center).padding({ top: 4 })
            .position({ x: 0, y: 0 })

            // ---- Settings panel overlay ----
            if (this.showSettings) {
                // Semi-transparent backdrop – tap to close
                Column()
                    .width('100%').height('100%')
                    .backgroundColor('rgba(0,0,0,0.35)')
                    .onClick(() => { this.showSettings = false })

                // Settings card
                Column() {
                    Text('参数设置').fontSize(14).fontColor(Color.White).padding({ top: 8, bottom: 4 })

                    this.SettingSlider('薄膜厚度', 740, 100, 2000, 10, (v) => { try { testNapi.setThickness(v) } catch (e) {} })
                    this.SettingSlider('厚度扰动', 160, 0, 500, 10, (v) => { try { testNapi.setThicknessVar(v) } catch (e) {} })
                    this.SettingSlider('表面张力', 15, 1, 50, 1, (v) => { try { testNapi.setSurfaceTension(v) } catch (e) {} })
                    this.SettingSlider('菲涅尔', 8, 0.5, 20, 0.5, (v) => { try { testNapi.setFresnelPower(v) } catch (e) {} })
                    this.SettingSlider('折射强度', 0.35, 0, 4, 0.05, (v) => { try { testNapi.setRefractionStrength(v) } catch (e) {} })
                    this.SettingSlider('边缘畸变', 2.2, 1, 3, 0.1, (v) => { try { testNapi.setEdgeDistortionBoost(v) } catch (e) {} })
                    this.SettingSlider('环境反射', 0.65, 0, 1.5, 0.05, (v) => { try { testNapi.setEnvironmentReflectionStrength(v) } catch (e) {} })
                    this.SettingSlider('相机距离', 7, 3.5, 20, 0.5, (v) => { try { testNapi.setCameraDistance(v) } catch (e) {} })

                    Row() {
                        Button('关闭').fontSize(12).width(80).height(32)
                            .onClick(() => { this.showSettings = false })
                        Button('重置').fontSize(12).width(80).height(32)
                            .onClick(() => {
                                try { testNapi.resetSimulation() } catch (e) {}
                            })
                    }.width('100%').justifyContent(FlexAlign.SpaceEvenly).padding({ top: 8, bottom: 8 })
                }
                .width('85%')
                .backgroundColor('rgba(30,30,30,0.92)')
                .borderRadius(12)
                .padding({ left: 12, right: 12, top: 8, bottom: 8 })
                .position({ x: '7.5%', y: '25%' })
            }
        }
        .onTouch((event: TouchEvent) => {
            this.mapAndDispatchTouch(event)
        })
        .height('100%')
    }
}
