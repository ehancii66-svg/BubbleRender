declare namespace libentry {
    /** Initialize OpenGL/EGL rendering context. */
    function initGraphics(resourceManager: object, surfaceId: string, fileDir: string): boolean;

    /** Render one frame. */
    function renderFrame(): void;

    /** Current averaged render frame rate. */
    function getFps(): number;

    /** Forward touch event to native, type: 0=Down, 1=Up, 2=Move. */
    function onTouch(x: number, y: number, type: number): void;

    function setInputViewport(width: number, height: number): void;

    function releaseResource(): void;

    /** Iridescence mode: 0=Kim2012, 1=Spectral LUT, 2=Belcour Airy. */
    function setIridescenceMode(mode: number): void;
    function setSurfaceTension(val: number): void;
    function setRefractionStrength(val: number): void;
    function resetSimulation(): void;

    function rotateCamera(dx: number, dy: number): void;

    /** Fresnel edge power (0.5 – 20, default 8). */
    function setFresnelPower(val: number): void;
    /** Edge distortion boost (1.0 – 3.0, default 2.2). */
    function setEdgeDistortionBoost(val: number): void;
    /** Max refraction offset ratio (0.1 – 1.0, default 0.52). */
    function setMaxOffsetRatio(val: number): void;
    /** Environment reflection strength (0.0 – 1.5, default 0.65). */
    function setEnvironmentReflectionStrength(val: number): void;
    /** Camera orbit distance (2.0-20, opening default 3.75). */
    function setCameraDistance(val: number): void;

    /** Pause / resume the DBSTT simulation. */
    function toggleSimulation(): void;
    /** Thin-film thickness in nm (100 – 2000, default 350 / 740). */
    function setThickness(val: number): void;
    /** Thickness variation amplitude in nm (0 – 500, default 160). */
    function setThicknessVar(val: number): void;

    /** Restore the multi-bubble opening scene (desktop X equivalent). */
    function resetOpeningScene(): void;
    /** Cycle stable double, unequal fusion and separation demos (desktop G equivalent). */
    function cycleInteractionDemo(): number;
    function setWindStrength(val: number): void;
    function addBubbleAtScreen(x: number, y: number, radius: number, depth: number): boolean;
    function burstBubbleAtScreen(x: number, y: number): boolean;
}
export default libentry;
