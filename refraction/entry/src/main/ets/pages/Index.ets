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
    private initRetryTimerId: number = -1
    private fpsUpdateCounter: number = 0
    private surfaceLoaded: boolean = false
    private nativeReady: boolean = false
    private nativeResourceManager?: object
    private nativeSurfaceId: string = ''
    private nativeFileDir: string = ''

    @State private compX: number = 0;
    @State private compY: number = 0;
    @State private compW: number = 0;
    @State private compH: number = 0;
    @State private controlsX: number = 0;
    @State private controlsY: number = 0;
    @State private controlsW: number = 0;
    @State private controlsH: number = 0;
    @State private menuX: number = 0;
    @State private menuY: number = 0;
    @State private menuW: number = 0;
    @State private menuH: number = 0;
    @State private showControls: boolean = false;
    @State private showSettings: boolean = false;
    @State private showInteractionHint: boolean = false;
    @State private fpsText: string = 'FPS: --  RS: 0.50';
    @State private modeText: string = 'Airy';
    @State private simulationText: string = '暂停仿真';
    @State private simPaused: boolean = false;
    @State private cameraDistance: number = 4.0;
    @State private thickness: number = 740;
    @State private thicknessVar: number = 160;
    @State private surfaceTension: number = 15;
    @State private refractionStrength: number = 0.35;
    @State private edgeDistortionBoost: number = 2.2;
    @State private environmentReflection: number = 0.65;
    @State private touchMode: number = 0;
    @State private bubbleRadius: number = 0.48;
    @State private spawnDepth: number = 0.0;
    @State private windStrength: number = 0.0;
    @State private windDirection: number = 0;
    @State private demoText: string = '演示';

    private lastTouchX: number = 0
    private lastTouchY: number = 0
    private touchMoved: boolean = false
    private pinchDistance: number = 0
    private pinchCameraDistance: number = 4.0
    private suppressSingleTouchAfterPinch: boolean = false
    private lastTapTime: number = 0
    private lastTapX: number = 0
    private lastTapY: number = 0
    private suppressToolGestureUntilUp: boolean = false

    private distanceBetweenTouches(a: TouchObject, b: TouchObject): number {
        const dx = a.x - b.x
        const dy = a.y - b.y
        return Math.sqrt(dx * dx + dy * dy)
    }

    private isInsideControls(x: number, y: number): boolean {
        const insidePanel = this.showControls &&
            this.controlsW > 0 && this.controlsH > 0 &&
            x >= this.controlsX && x <= this.controlsX + this.controlsW &&
            y >= this.controlsY && y <= this.controlsY + this.controlsH
        const insideMenu = this.menuW > 0 && this.menuH > 0 &&
            x >= this.menuX && x <= this.menuX + this.menuW &&
            y >= this.menuY && y <= this.menuY + this.menuH
        return insidePanel || insideMenu
    }

    private setCameraDistanceValue(value: number): void {
        this.cameraDistance = Math.min(20.0, Math.max(2.0, value))
        try {
            testNapi.setCameraDistance(this.cameraDistance)
        } catch (e) {
            hilog.error(DOMAIN, 'Index', 'setCameraDistance failed: %{public}s', JSON.stringify(e))
        }
    }

    private selectTouchMode(mode: number): void {
        this.hideAddPreview()
        this.touchMode = this.touchMode === mode ? 0 : mode
        this.showControls = false
    }

    private toggleControlMenu(): void {
        if (this.showSettings) {
            this.showSettings = false
            this.showControls = false
            return
        }
        this.showControls = !this.showControls
    }

    private hideAddPreview(): void {
        try {
            testNapi.setAddPreview(0, 0, this.bubbleRadius, this.spawnDepth, false)
        } catch (e) {}
    }

    private updateAddPreview(x: number, y: number): void {
        try {
            testNapi.setAddPreview(x, y, this.bubbleRadius, this.spawnDepth, true)
        } catch (e) {
            hilog.error(DOMAIN, 'Index', 'setAddPreview failed: %{public}s', JSON.stringify(e))
        }
    }

    private resetCameraView(): void {
        try {
            this.cameraDistance = testNapi.resetCameraView()
            this.pinchCameraDistance = this.cameraDistance
        } catch (e) {
            hilog.error(DOMAIN, 'Index', 'resetCameraView failed: %{public}s', JSON.stringify(e))
        }
    }

    private handleOrbitTap(localX: number, localY: number): boolean {
        const now = Date.now()
        const dx = localX - this.lastTapX
        const dy = localY - this.lastTapY
        const isDoubleTap = now - this.lastTapTime <= 320 && dx * dx + dy * dy <= 24 * 24
        this.lastTapTime = now
        this.lastTapX = localX
        this.lastTapY = localY
        if (!isDoubleTap) {
            return false
        }
        try {
            if (!testNapi.hasBubbleAtScreen(localX, localY)) {
                this.resetCameraView()
                this.lastTapTime = 0
                return true
            }
        } catch (e) {
            hilog.error(DOMAIN, 'Index', 'hasBubbleAtScreen failed: %{public}s', JSON.stringify(e))
        }
        return false
    }

    private setMode(mode: number, label: string): void {
        this.modeText = label
        try {
            testNapi.setIridescenceMode(mode)
        } catch (e) {
            hilog.error(DOMAIN, 'Index', 'setIridescenceMode failed: %{public}s', JSON.stringify(e))
        }
    }

    private toggleSimulationState(): void {
        this.simPaused = !this.simPaused
        this.simulationText = this.simPaused ? '继续仿真' : '暂停仿真'
        try {
            testNapi.toggleSimulation()
        } catch (e) {
            hilog.error(DOMAIN, 'Index', 'toggleSimulation failed: %{public}s', JSON.stringify(e))
        }
    }

    private resetOpeningScene(): void {
        try {
            testNapi.resetOpeningScene()
            this.demoText = '演示'
            this.cameraDistance = 4.0
            this.pinchCameraDistance = this.cameraDistance
            this.touchMode = 0
            this.hideAddPreview()
        } catch (e) {
            hilog.error(DOMAIN, 'Index', 'resetOpeningScene failed: %{public}s', JSON.stringify(e))
        }
    }

    private cycleInteractionDemo(): void {
        try {
            const mode = testNapi.cycleInteractionDemo()
            const labels: string[] = ['8字', '融合', '分离']
            this.demoText = labels[Math.max(0, Math.min(2, mode))]
            this.cameraDistance = 5.10
            this.pinchCameraDistance = this.cameraDistance
            this.touchMode = 0
            this.hideAddPreview()
        } catch (e) {
            hilog.error(DOMAIN, 'Index', 'cycleInteractionDemo failed: %{public}s', JSON.stringify(e))
        }
    }

    private toggleWind(): void {
        this.windStrength = this.windStrength > 0 ? 0 : 0.12
        try {
            testNapi.setWindStrength(this.windStrength)
        } catch (e) {
            hilog.error(DOMAIN, 'Index', 'setWindStrength failed: %{public}s', JSON.stringify(e))
        }
    }

    private setWindDirection(index: number, x: number, y: number): void {
        this.windDirection = index
        try {
            testNapi.setWindDirection(x, y)
        } catch (e) {
            hilog.error(DOMAIN, 'Index', 'setWindDirection failed: %{public}s', JSON.stringify(e))
        }
    }

    private resetAllParameters(): void {
        this.thickness = 740
        this.thicknessVar = 160
        this.surfaceTension = 15
        this.refractionStrength = 0.35
        this.edgeDistortionBoost = 2.2
        this.environmentReflection = 0.65
        this.cameraDistance = 4.0
        this.touchMode = 0
        this.bubbleRadius = 0.48
        this.spawnDepth = 0.0
        this.windStrength = 0.0
        this.windDirection = 0
        this.demoText = 'Demo'
        this.simPaused = false
        this.simulationText = '暂停仿真'

        try {
            testNapi.resetSimulation()
            testNapi.setThickness(this.thickness)
            testNapi.setThicknessVar(this.thicknessVar)
            testNapi.setSurfaceTension(this.surfaceTension)
            testNapi.setRefractionStrength(this.refractionStrength)
            testNapi.setEdgeDistortionBoost(this.edgeDistortionBoost)
            testNapi.setEnvironmentReflectionStrength(this.environmentReflection)
            testNapi.setWindDirection(0.92, 0.38)
            testNapi.setCameraDistance(this.cameraDistance)
        } catch (e) {
            hilog.error(DOMAIN, 'Index', 'resetAllParameters failed: %{public}s', JSON.stringify(e))
        }
    }

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

    private scheduleInitRetry(): void {
        if (this.initRetryTimerId >= 0 || this.nativeReady) {
            return
        }
        this.initRetryTimerId = setTimeout(() => {
            this.initRetryTimerId = -1
            this.tryInitGraphics()
        }, 120)
    }

    private tryInitGraphics(): void {
        if (this.nativeReady) {
            return
        }
        if (!this.surfaceLoaded || !this.nativeResourceManager || this.nativeSurfaceId.length === 0 ||
            this.compW <= 0 || this.compH <= 0) {
            hilog.info(DOMAIN, 'Index',
                `wait native init surfaceLoaded=${this.surfaceLoaded}, area=${this.compW}x${this.compH}`)
            this.scheduleInitRetry()
            return
        }

        try {
            if (this.initRetryTimerId >= 0) {
                clearTimeout(this.initRetryTimerId)
                this.initRetryTimerId = -1
            }
            testNapi.setInputViewport(this.compW, this.compH)
            const ok = testNapi.initGraphics(this.nativeResourceManager, this.nativeSurfaceId, this.nativeFileDir)
            if (ok) {
                this.nativeReady = true
                this.fpsText = 'FPS: --  RS: 0.50'
                hilog.info(DOMAIN, 'Index', 'native graphics initialized')
                this.startRenderLoop()
            } else {
                hilog.warn(DOMAIN, 'Index', 'native graphics init returned false, retry later')
                this.scheduleInitRetry()
            }
        } catch (err) {
            hilog.error(DOMAIN, 'Index', 'initGraphics failed: %{public}s', JSON.stringify(err))
            this.scheduleInitRetry()
        }
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

        if (this.isInsideControls(points[0].x, points[0].y)) {
            return
        }
        if (this.showControls && event.type === TouchType.Down) {
            this.showControls = false
            return
        }

        if (points.length >= 2) {
            this.hideAddPreview()
            this.suppressSingleTouchAfterPinch = true
            const currentPinch = this.distanceBetweenTouches(points[0], points[1])
            if (event.type === TouchType.Down || this.pinchDistance <= 1) {
                this.pinchDistance = currentPinch
                this.pinchCameraDistance = this.cameraDistance
            } else if (event.type === TouchType.Move && currentPinch > 1) {
                const ratio = this.pinchDistance / currentPinch
                this.setCameraDistanceValue(this.pinchCameraDistance * ratio)
            } else if (event.type === TouchType.Up) {
                this.pinchDistance = 0
            }
            return
        }

        const touch = points[0]
        let touchType = 2
        switch (event.type) {
            case TouchType.Down:
                this.suppressSingleTouchAfterPinch = false
                touchType = 0
                this.lastTouchX = touch.x
                this.lastTouchY = touch.y
                this.touchMoved = false
                this.pinchDistance = 0
                break
            case TouchType.Up:
                touchType = 1
                break
            case TouchType.Move:
                touchType = 2
                break
        }
        if (this.suppressSingleTouchAfterPinch) {
            return
        }
        if (this.suppressToolGestureUntilUp) {
            if (touchType === 1) {
                this.suppressToolGestureUntilUp = false
            }
            return
        }
        const hasArea = this.compW > 0 && this.compH > 0
        const localX = hasArea ? (touch.x - this.compX) : touch.x
        const localY = hasArea ? (touch.y - this.compY) : touch.y

        const inComp = hasArea ? (localX >= 0 && localX <= this.compW && localY >= 0 && localY <= this.compH) : true
        if (!inComp && touchType !== 1) {
            return
        }

        if (touchType === 2 && this.touchMode !== 1) {
            const moveX = touch.x - this.lastTouchX
            const moveY = touch.y - this.lastTouchY
            if (Math.abs(moveX) > 3 || Math.abs(moveY) > 3) {
                this.touchMoved = true
            }
        }

        const activeMode = this.touchMode

        if (activeMode === 1) {
            if ((touchType === 0 || touchType === 2) && inComp) {
                this.updateAddPreview(localX, localY)
            } else if (touchType === 1) {
                this.hideAddPreview()
            }
            if (touchType === 1 && inComp) {
                try {
                    testNapi.addBubbleAtScreen(localX, localY, this.bubbleRadius, this.spawnDepth)
                } catch (err) {
                    hilog.error(DOMAIN, 'Index', 'add bubble failed: %{public}s', JSON.stringify(err))
                }
            }
            return
        }

        if (activeMode === 2) {
            if (touchType === 0 && inComp) {
                try {
                    if (testNapi.burstBubbleAtScreen(localX, localY)) {
                        this.suppressToolGestureUntilUp = true
                    }
                } catch (err) {
                    hilog.error(DOMAIN, 'Index', 'burst bubble failed: %{public}s', JSON.stringify(err))
                }
            }
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

        if (touchType === 1 && !this.touchMoved && inComp) {
            this.handleOrbitTap(localX, localY)
        }

        try {
            testNapi.onTouch(localX, localY, touchType)
        } catch (err) {
            hilog.error(DOMAIN, 'Index', 'call onTouch failed: %{public}s', JSON.stringify(err))
        }
    }

    aboutToAppear(): void {}

    aboutToDisappear(): void {
        this.stopRenderLoop()
        if (this.initRetryTimerId >= 0) {
            clearTimeout(this.initRetryTimerId)
            this.initRetryTimerId = -1
        }
        this.nativeReady = false
        this.surfaceLoaded = false
        testNapi.releaseResource()
    }

    @Builder SettingSlider(label: string, value: number, minVal: number, maxVal: number,
                           stepVal: number, digits: number, onChange: (val: number) => void) {
        Column() {
            Row() {
                Text(label).fontSize(12).fontColor(Color.White).width(88)
                Text(value.toFixed(digits))
                    .fontSize(11)
                    .fontColor('rgba(255,255,255,0.72)')
                    .textAlign(TextAlign.End)
                    .width(48)
            }.width('100%').justifyContent(FlexAlign.SpaceBetween)
            Slider({ value: value, min: minVal, max: maxVal, step: stepVal, style: SliderStyle.OutSet })
                .width('100%').height(28)
                .onChange((val: number, _mode: SliderChangeMode) => { onChange(val) })
        }.width('100%').padding({ top: 4, bottom: 4 })
    }

    @Builder ParamSection(title: string) {
        Text(title)
            .fontSize(12)
            .fontColor('rgba(255,255,255,0.92)')
            .fontWeight(FontWeight.Medium)
            .width('100%')
            .padding({ top: 8, bottom: 2 })
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
                    this.nativeResourceManager = context.resourceManager
                    this.nativeSurfaceId = this.xComponentController.getXComponentSurfaceId()
                    this.nativeFileDir = fileDir
                    this.surfaceLoaded = true
                    this.tryInitGraphics()
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
                    this.tryInitGraphics()
                })
                .width('100%')
                .height('100%')

            if (this.showControls || this.showSettings) {
                Row() {
                    Text(this.fpsText)
                        .fontSize(11)
                        .fontColor('rgba(255,255,255,0.82)')
                    Text(this.modeText)
                        .fontSize(11)
                        .fontColor('rgba(255,255,255,0.58)')
                        .padding({ left: 10 })
                }
                .height(30)
                .padding({ left: 10, right: 10 })
                .backgroundColor('rgba(10,18,24,0.66)')
                .border({ width: 1, color: 'rgba(255,255,255,0.12)' })
                .borderRadius(6)
                .position({ x: 10, y: 10 })
            }

            if (this.showInteractionHint && !this.showSettings) {
                Column() {
                    Text('交互提示')
                        .fontSize(14)
                        .fontColor(Color.White)
                        .fontWeight(FontWeight.Medium)
                        .padding({ bottom: 6 })
                    Text('单指拖动旋转视角，点击泡泡产生扰动，双指缩放相机距离。')
                        .fontSize(12)
                        .fontColor('rgba(255,255,255,0.78)')
                        .lineHeight(18)
                    Row() {
                        Button('知道了')
                            .fontSize(12)
                            .height(30)
                            .onClick(() => { this.showInteractionHint = false })
                    }
                    .width('100%')
                    .justifyContent(FlexAlign.End)
                    .padding({ top: 8 })
                }
                .width('78%')
                .padding({ left: 12, right: 12, top: 10, bottom: 10 })
                .backgroundColor('rgba(18,18,18,0.66)')
                .borderRadius(8)
                .position({ x: '11%', y: '10%' })
            }

            Button() {
                Text(this.showControls || this.showSettings ? '×' : '≡')
                    .fontSize(19)
                    .fontColor(Color.White)
                    .width('100%')
                    .height('100%')
                    .textAlign(TextAlign.Center)
            }
                .width(40)
                .height(40)
                .padding(0)
                .backgroundColor('rgba(10,18,24,0.72)')
                .border({ width: 1, color: 'rgba(255,255,255,0.16)' })
                .borderRadius(8)
                .position({ x: '82%', y: 10 })
                .onClick(() => { this.toggleControlMenu() })
                .onAreaChange((_oldValue: Area, newValue: Area) => {
                    this.menuX = Number(newValue.position.x)
                    this.menuY = Number(newValue.position.y)
                    this.menuW = Number(newValue.width)
                    this.menuH = Number(newValue.height)
                })

            if (this.showControls) {
                Column() {
                Row() {
                    Text('场景控制')
                        .fontSize(14)
                        .fontWeight(FontWeight.Medium)
                        .fontColor(Color.White)
                    Text(this.windStrength > 0 ? '风已开启' : '无风')
                        .fontSize(11)
                        .fontColor(this.windStrength > 0 ? '#8ED7C5' : 'rgba(255,255,255,0.52)')
                }
                .width('100%')
                .height(28)
                .justifyContent(FlexAlign.SpaceBetween)

                Row() {
                    Button('场景')
                        .fontSize(12).height(34).layoutWeight(1).borderRadius(6)
                        .backgroundColor('#355D72')
                        .onClick(() => { this.resetOpeningScene() })
                    Button(this.demoText)
                        .fontSize(12).height(34).layoutWeight(1).borderRadius(6)
                        .backgroundColor('#355D72')
                        .onClick(() => { this.cycleInteractionDemo() })
                    Button(this.windStrength > 0 ? '关闭风' : '开启风')
                        .fontSize(12).height(34).layoutWeight(1).borderRadius(6)
                        .backgroundColor(this.windStrength > 0 ? '#2D8B78' : '#4A555C')
                        .onClick(() => { this.toggleWind() })
                }
                .width('100%')
                .justifyContent(FlexAlign.SpaceBetween)

                if (this.windStrength > 0) {
                    Row() {
                        Text('风向')
                            .fontSize(11)
                            .fontColor('rgba(255,255,255,0.72)')
                            .width(42)
                        Button('↖')
                            .fontSize(18).height(32).layoutWeight(1).borderRadius(6)
                            .backgroundColor(this.windDirection === 1 ? '#2D8B78' : '#39464D')
                            .onClick(() => { this.setWindDirection(1, -0.92, 0.38) })
                        Button('↗')
                            .fontSize(18).height(32).layoutWeight(1).borderRadius(6)
                            .backgroundColor(this.windDirection === 0 ? '#2D8B78' : '#39464D')
                            .onClick(() => { this.setWindDirection(0, 0.92, 0.38) })
                        Button('↙')
                            .fontSize(18).height(32).layoutWeight(1).borderRadius(6)
                            .backgroundColor(this.windDirection === 2 ? '#2D8B78' : '#39464D')
                            .onClick(() => { this.setWindDirection(2, -0.92, -0.38) })
                        Button('↘')
                            .fontSize(18).height(32).layoutWeight(1).borderRadius(6)
                            .backgroundColor(this.windDirection === 3 ? '#2D8B78' : '#39464D')
                            .onClick(() => { this.setWindDirection(3, 0.92, -0.38) })
                    }
                    .width('100%')
                    .padding({ top: 6 })
                }

                Row() {
                    Button('旋转')
                        .fontSize(12).height(32).layoutWeight(1).borderRadius(6)
                        .backgroundColor(this.touchMode === 0 ? '#2878D0' : '#505050')
                        .onClick(() => {
                            this.hideAddPreview()
                            this.touchMode = 0
                            this.showControls = false
                        })
                    Button(this.touchMode === 1 ? '取消添加' : '添加')
                        .fontSize(12).height(32).layoutWeight(1).borderRadius(6)
                        .backgroundColor(this.touchMode === 1 ? '#2878D0' : '#505050')
                        .onClick(() => { this.selectTouchMode(1) })
                    Button(this.touchMode === 2 ? '取消破裂' : '破裂')
                        .fontSize(12).height(32).layoutWeight(1).borderRadius(6)
                        .backgroundColor(this.touchMode === 2 ? '#B43A48' : '#505050')
                        .onClick(() => { this.selectTouchMode(2) })
                }
                .width('100%')
                .justifyContent(FlexAlign.SpaceBetween)
                .padding({ top: 5 })

                if (this.touchMode === 1) {
                    Column() {
                        Row() {
                            Text('泡泡大小')
                                .fontSize(11)
                                .fontColor(Color.White)
                                .width(72)
                            Slider({ value: this.bubbleRadius, min: 0.16, max: 1.10, step: 0.02,
                                style: SliderStyle.OutSet })
                                .height(28)
                                .layoutWeight(1)
                                .onChange((v: number, _mode: SliderChangeMode) => { this.bubbleRadius = v })
                            Text(this.bubbleRadius.toFixed(2))
                                .fontSize(11)
                                .fontColor('rgba(255,255,255,0.76)')
                                .width(36)
                                .textAlign(TextAlign.End)
                        }
                        .width('100%')

                        Row() {
                            Text('添加深度')
                                .fontSize(11)
                                .fontColor(Color.White)
                                .width(72)
                            Button('近')
                                .fontSize(11).height(28).layoutWeight(1)
                                .backgroundColor(this.spawnDepth === 2.0 ? '#2878D0' : '#505050')
                                .onClick(() => { this.spawnDepth = 2.0 })
                            Button('中')
                                .fontSize(11).height(28).layoutWeight(1)
                                .backgroundColor(this.spawnDepth === 0.0 ? '#2878D0' : '#505050')
                                .onClick(() => { this.spawnDepth = 0.0 })
                            Button('远')
                                .fontSize(11).height(28).layoutWeight(1)
                                .backgroundColor(this.spawnDepth === -2.0 ? '#2878D0' : '#505050')
                                .onClick(() => { this.spawnDepth = -2.0 })
                        }
                        .width('100%')
                    }
                    .width('100%')
                    .padding({ top: 4, bottom: 2 })
                }

                Row() {
                    Button('Kim')
                        .fontSize(12)
                        .height(34)
                        .layoutWeight(1)
                        .borderRadius(6)
                        .backgroundColor(this.modeText === 'Kim' ? '#2878D0' : '#4A555C')
                        .onClick(() => { this.setMode(0, 'Kim') })
                    Button('Airy')
                        .fontSize(12)
                        .height(34)
                        .layoutWeight(1)
                        .borderRadius(6)
                        .backgroundColor(this.modeText === 'Airy' ? '#2878D0' : '#4A555C')
                        .onClick(() => { this.setMode(2, 'Airy') })
                }
                .width('100%')
                .justifyContent(FlexAlign.SpaceBetween)

                Row() {
                    Button(this.simulationText)
                        .fontSize(12)
                        .height(34)
                        .layoutWeight(1)
                        .borderRadius(6)
                        .backgroundColor('#4A555C')
                        .onClick(() => { this.toggleSimulationState() })
                    Button(this.showSettings ? '收起参数' : '参数')
                        .fontSize(12)
                        .height(34)
                        .layoutWeight(1)
                        .borderRadius(6)
                        .backgroundColor('#4A555C')
                        .onClick(() => {
                            this.touchMode = 0
                            this.hideAddPreview()
                            this.showSettings = true
                            this.showControls = false
                        })
                    Button('重置')
                        .fontSize(12)
                        .height(34)
                        .layoutWeight(1)
                        .borderRadius(6)
                        .backgroundColor('#4A555C')
                        .onClick(() => { this.resetAllParameters() })
                }
                .width('100%')
                .justifyContent(FlexAlign.SpaceBetween)
                .padding({ top: 6 })
                }
                .width('92%')
                .padding({ left: 10, right: 10, top: 8, bottom: 10 })
                .backgroundColor('rgba(10,18,24,0.86)')
                .border({ width: 1, color: 'rgba(255,255,255,0.13)' })
                .borderRadius(8)
                .position({ x: '4%', y: 60 })
                .onAreaChange((_oldValue: Area, newValue: Area) => {
                    this.controlsX = Number(newValue.position.x)
                    this.controlsY = Number(newValue.position.y)
                    this.controlsW = Number(newValue.width)
                    this.controlsH = Number(newValue.height)
                })
            }

            if (this.showSettings) {
                Column() {
                    Text('参数调节')
                        .fontSize(15)
                        .fontColor(Color.White)
                        .fontWeight(FontWeight.Medium)
                        .padding({ bottom: 6 })

                    this.ParamSection('泡泡与环境')
                    this.SettingSlider('泡泡大小', this.bubbleRadius, 0.16, 1.10, 0.02, 2,
                        (v: number) => { this.bubbleRadius = v })
                    this.SettingSlider('添加深度', this.spawnDepth, -3.0, 2.0, 0.1, 1,
                        (v: number) => { this.spawnDepth = v })
                    this.SettingSlider('风速', this.windStrength, 0.0, 0.45, 0.01, 2,
                        (v: number) => {
                            this.windStrength = v
                            try { testNapi.setWindStrength(v) } catch (e) {}
                        })

                    this.ParamSection('光学参数')
                    Column() {
                        Row() {
                            Text('干涉膜厚').fontSize(12).fontColor(Color.White).width(88)
                            Text(this.thickness.toFixed(0))
                                .fontSize(11)
                                .fontColor('rgba(255,255,255,0.72)')
                                .textAlign(TextAlign.End)
                                .width(48)
                        }.width('100%').justifyContent(FlexAlign.SpaceBetween)
                        Slider({ value: this.thickness, min: 100, max: 2000, step: 10, style: SliderStyle.OutSet })
                            .width('100%').height(28)
                            .onChange((v: number, _mode: SliderChangeMode) => {
                                this.thickness = v
                                try { testNapi.setThickness(v) } catch (e) {}
                            })
                    }.width('100%').padding({ top: 4, bottom: 4 })
                    Column() {
                        Row() {
                            Text('折射强度').fontSize(12).fontColor(Color.White).width(88)
                            Text(this.refractionStrength.toFixed(2))
                                .fontSize(11)
                                .fontColor('rgba(255,255,255,0.72)')
                                .textAlign(TextAlign.End)
                                .width(48)
                        }.width('100%').justifyContent(FlexAlign.SpaceBetween)
                        Slider({ value: this.refractionStrength, min: 0, max: 4, step: 0.05, style: SliderStyle.OutSet })
                            .width('100%').height(28)
                            .onChange((v: number, _mode: SliderChangeMode) => {
                                this.refractionStrength = v
                                try { testNapi.setRefractionStrength(v) } catch (e) {}
                            })
                    }.width('100%').padding({ top: 4, bottom: 4 })
                    Column() {
                        Row() {
                            Text('环境反射').fontSize(12).fontColor(Color.White).width(88)
                            Text(this.environmentReflection.toFixed(2))
                                .fontSize(11)
                                .fontColor('rgba(255,255,255,0.72)')
                                .textAlign(TextAlign.End)
                                .width(48)
                        }.width('100%').justifyContent(FlexAlign.SpaceBetween)
                        Slider({ value: this.environmentReflection, min: 0, max: 1.5, step: 0.05, style: SliderStyle.OutSet })
                            .width('100%').height(28)
                            .onChange((v: number, _mode: SliderChangeMode) => {
                                this.environmentReflection = v
                                try { testNapi.setEnvironmentReflectionStrength(v) } catch (e) {}
                            })
                    }.width('100%').padding({ top: 4, bottom: 4 })

                    this.ParamSection('仿真参数')
                    Column() {
                        Row() {
                            Text('厚度扰动').fontSize(12).fontColor(Color.White).width(88)
                            Text(this.thicknessVar.toFixed(0))
                                .fontSize(11)
                                .fontColor('rgba(255,255,255,0.72)')
                                .textAlign(TextAlign.End)
                                .width(48)
                        }.width('100%').justifyContent(FlexAlign.SpaceBetween)
                        Slider({ value: this.thicknessVar, min: 0, max: 500, step: 10, style: SliderStyle.OutSet })
                            .width('100%').height(28)
                            .onChange((v: number, _mode: SliderChangeMode) => {
                                this.thicknessVar = v
                                try { testNapi.setThicknessVar(v) } catch (e) {}
                            })
                    }.width('100%').padding({ top: 4, bottom: 4 })
                    Column() {
                        Row() {
                            Text('表面张力').fontSize(12).fontColor(Color.White).width(88)
                            Text(this.surfaceTension.toFixed(0))
                                .fontSize(11)
                                .fontColor('rgba(255,255,255,0.72)')
                                .textAlign(TextAlign.End)
                                .width(48)
                        }.width('100%').justifyContent(FlexAlign.SpaceBetween)
                        Slider({ value: this.surfaceTension, min: 1, max: 50, step: 1, style: SliderStyle.OutSet })
                            .width('100%').height(28)
                            .onChange((v: number, _mode: SliderChangeMode) => {
                                this.surfaceTension = v
                                try { testNapi.setSurfaceTension(v) } catch (e) {}
                            })
                    }.width('100%').padding({ top: 4, bottom: 4 })
                    Column() {
                        Row() {
                            Text('边缘畸变').fontSize(12).fontColor(Color.White).width(88)
                            Text(this.edgeDistortionBoost.toFixed(1))
                                .fontSize(11)
                                .fontColor('rgba(255,255,255,0.72)')
                                .textAlign(TextAlign.End)
                                .width(48)
                        }.width('100%').justifyContent(FlexAlign.SpaceBetween)
                        Slider({ value: this.edgeDistortionBoost, min: 1, max: 3, step: 0.1, style: SliderStyle.OutSet })
                            .width('100%').height(28)
                            .onChange((v: number, _mode: SliderChangeMode) => {
                                this.edgeDistortionBoost = v
                                try { testNapi.setEdgeDistortionBoost(v) } catch (e) {}
                            })
                    }.width('100%').padding({ top: 4, bottom: 4 })

                    this.ParamSection('性能与视图')
                    Column() {
                        Row() {
                            Text('相机距离').fontSize(12).fontColor(Color.White).width(88)
                            Text(this.cameraDistance.toFixed(1))
                                .fontSize(11)
                                .fontColor('rgba(255,255,255,0.72)')
                                .textAlign(TextAlign.End)
                                .width(48)
                        }.width('100%').justifyContent(FlexAlign.SpaceBetween)
                        Slider({ value: this.cameraDistance, min: 2.0, max: 20, step: 0.25, style: SliderStyle.OutSet })
                            .width('100%').height(28)
                            .onChange((v: number, _mode: SliderChangeMode) => {
                                this.setCameraDistanceValue(v)
                            })
                    }.width('100%').padding({ top: 4, bottom: 4 })
                    Button('完成')
                        .fontSize(13)
                        .height(34)
                        .width('100%')
                        .margin({ top: 8 })
                        .onClick(() => { this.showSettings = false })
                }
                .width('92%')
                .backgroundColor('rgba(24,24,24,0.82)')
                .borderRadius(8)
                .padding({ left: 14, right: 14, top: 12, bottom: 12 })
                .position({ x: '4%', y: '18%' })
            }
        }
        .height('100%')
        .onTouch((event: TouchEvent) => {
            this.mapAndDispatchTouch(event)
        })
    }
}
