import * as THREE from "three";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";

let renderer;
let sensors = [];
let pointSize = 0.03;

const pointSizeSlider = document.getElementById("point-size-slider");
const pointSizeValue = document.getElementById("point-size-value");
const statusLabel = document.getElementById("status");
const gridContainer = document.getElementById("viewport-grid");

init();
loadMetadata();

function init() {
  renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.setPixelRatio(window.devicePixelRatio);
  renderer.setSize(window.innerWidth, window.innerHeight);
  renderer.setScissorTest(true);
  document.getElementById("canvas-container").appendChild(renderer.domElement);

  window.addEventListener("resize", onWindowResize);
  pointSizeSlider.addEventListener("input", (e) => {
    pointSize = parseFloat(e.target.value);
    pointSizeValue.innerText = pointSize.toFixed(3);
    sensors.forEach((sensor) => {
      sensor.points.material.size = pointSize;
      sensor.points.material.needsUpdate = true;
    });
  });
  animate();
}

function toVector3(values, fallback = [0, 0, 0]) {
  const source =
    Array.isArray(values) && values.length >= 3 ? values : fallback;
  return new THREE.Vector3(source[0], source[1], source[2]);
}

function getSensorPose(sensor) {
  const transform = sensor.transform || {};
  const position = toVector3(transform.pos);
  const rot = Array.isArray(transform.rot) ? transform.rot : [0, 0, 0];
  const rotation = new THREE.Euler(
    THREE.MathUtils.degToRad(rot[0] || 0),
    THREE.MathUtils.degToRad(rot[1] || 0),
    THREE.MathUtils.degToRad(rot[2] || 0),
    "XYZ",
  );
  return { position, rotation };
}

function resetSensorView(sensor) {
  if (sensor.cameraConfig) {
    sensor.camera.position.copy(sensor.cameraConfig.pos);
    sensor.camera.up.copy(sensor.cameraConfig.up);
    sensor.controls.target.copy(sensor.cameraConfig.target);
    sensor.controls.update();
    sensor.hasAutoView = true;
    return;
  }

  const positionAttr = sensor.geometry.attributes.position;
  if (!positionAttr || positionAttr.count === 0) return;

  const box = new THREE.Box3().setFromBufferAttribute(positionAttr);
  const center = box.getCenter(new THREE.Vector3());
  const size = box.getSize(new THREE.Vector3());
  const radius = Math.max(size.length() * 0.35, 3);

  const { position, rotation } = getSensorPose(sensor);
  const worldCenter = center.clone().applyEuler(rotation).add(position);
  const worldAxis = new THREE.Vector3(1, 0, 0).applyEuler(rotation).normalize();
  const up = new THREE.Vector3(0, 0, 1);

  const cameraPosition = worldCenter
    .clone()
    .add(worldAxis.clone().multiplyScalar(-radius * 1.8))
    .add(up.clone().multiplyScalar(radius * 0.15));

  sensor.camera.position.copy(cameraPosition);
  sensor.camera.up.copy(up);
  sensor.controls.target.copy(worldCenter);
  sensor.controls.update();
  sensor.hasAutoView = true;
}

async function loadMetadata() {
  try {
    const response = await fetch("data/metadata.json");
    const data = await response.json();

    // Initialize sensors
    sensors = data.sensors.map((s) => {
      // Create UI Container
      const container = document.createElement("div");
      container.className = "view-container";
      gridContainer.appendChild(container);

      const label = document.createElement("div");
      label.className = "view-label";
      label.innerText = `${s.name} (${s.model})`;
      container.appendChild(label);

      const controlsBar = document.createElement("div");
      controlsBar.className = "view-controls";
      controlsBar.innerHTML = `
        <div class="label-row">
          <div class="label">FRAME: <span class="sensor-frame-num">0</span> / <span class="sensor-total-frames">${Math.max(0, (s.source_frame_count ?? s.frames.length) - 1)}</span></div>
          <div class="label">PTS: <span class="sensor-points-count">0</span></div>
        </div>
      `;
      const frameSlider = document.createElement("input");
      frameSlider.type = "range";
      frameSlider.className = "sensor-slider";
      frameSlider.min = "0";
      frameSlider.max = String(Math.max(0, s.frames.length - 1));
      frameSlider.value = "0";
      controlsBar.appendChild(frameSlider);
      container.appendChild(controlsBar);

      // Create 3D Scene
      const scene = new THREE.Scene();
      const camera = new THREE.PerspectiveCamera(75, 1, 0.1, 1000);
      const cameraConfig = s.camera
        ? {
            pos: toVector3(s.camera.pos, [-12, 0, 2]),
            target: toVector3(s.camera.target, [4, 0, 0]),
            up: toVector3(s.camera.up, [0, 0, 1]),
          }
        : null;

      const controls = new OrbitControls(camera, container);
      controls.target.set(0, 0, 0);
      controls.enableDamping = true;
      controls.screenSpacePanning = false;
      controls.update();

      const grid = new THREE.GridHelper(100, 10, 0x444444, 0x222222);
      grid.rotation.x = Math.PI / 2;
      const sensorRoot = new THREE.Group();
      const { position, rotation } = getSensorPose(s);
      sensorRoot.position.copy(position);
      sensorRoot.rotation.copy(rotation);
      scene.add(sensorRoot);
      scene.add(grid);

      const geometry = new THREE.BufferGeometry();
      const material = new THREE.PointsMaterial({
        size: pointSize,
        vertexColors: true,
        sizeAttenuation: true,
      });
      const points = new THREE.Points(geometry, material);
      sensorRoot.add(points);

      return {
        ...s,
        container,
        scene,
        camera,
        cameraConfig,
        controls,
        sensorRoot,
        geometry,
        points,
        frameSlider,
        frameNumLabel: controlsBar.querySelector(".sensor-frame-num"),
        pointsCountLabel: controlsBar.querySelector(".sensor-points-count"),
        currentFrameIndex: -1,
        hasAutoView: false,
        visible: true,
      };
    });

    sensors.forEach((sensor) => {
      suppressViewportControls(sensor.frameSlider);
      sensor.frameSlider.addEventListener("input", (e) =>
        loadSensorFrame(sensor, parseInt(e.target.value)),
      );
    });

    const skippedCount = Array.isArray(data.skipped_sensors)
      ? data.skipped_sensors.length
      : 0;
    statusLabel.innerText =
      `${data.project_name || "Project"} loaded. ${sensors.length} separate views.` +
      (skippedCount > 0
        ? ` Skipped ${skippedCount} sensors with missing inputs.`
        : "");
    await Promise.all(sensors.map((sensor) => loadSensorFrame(sensor, 0)));
  } catch (e) {
    console.error("Error loading metadata:", e);
    statusLabel.innerText = "Error loading project metadata!";
  }
}

async function loadSensorFrame(sensor, index) {
  if (index === sensor.currentFrameIndex) return;
  if (index < 0 || index >= sensor.frames.length) return;

  sensor.currentFrameIndex = index;
  sensor.frameNumLabel.innerText = sensor.frames[index].source_index ?? index;
  sensor.frameSlider.value = String(index);

  try {
    const response = await fetch(`data/${sensor.frames[index].path}`);
    const data = await response.arrayBuffer();
    updatePoints(sensor, data);
  } catch (e) {
    console.warn(`Error loading frame for sensor ${sensor.name}:`, e);
  }
}

function updatePoints(sensor, data) {
  const points = new Float32Array(data);
  const pointCount = Math.floor(points.length / 4);
  const positions = new Float32Array(pointCount * 3);
  const colors = new Float32Array(pointCount * 3);

  for (let i = 0; i < pointCount; i += 1) {
    const src = i * 4;
    const dst = i * 3;
    positions[dst] = points[src];
    positions[dst + 1] = points[src + 1];
    positions[dst + 2] = points[src + 2];

    const intensity = Math.max(0, Math.min(1, points[src + 3] / 255.0));
    const color = intensityToColor(intensity);
    colors[dst] = color[0];
    colors[dst + 1] = color[1];
    colors[dst + 2] = color[2];
  }

  sensor.geometry.setAttribute(
    "position",
    new THREE.BufferAttribute(positions, 3),
  );
  sensor.geometry.setAttribute("color", new THREE.BufferAttribute(colors, 3));
  sensor.geometry.attributes.position.needsUpdate = true;
  sensor.geometry.attributes.color.needsUpdate = true;
  sensor.geometry.computeBoundingSphere();
  sensor.pointsCountLabel.innerText = pointCount;

  if (!sensor.hasAutoView) {
    resetSensorView(sensor);
  }
}

function suppressViewportControls(element) {
  const stop = (event) => {
    event.stopPropagation();
  };

  [
    "pointerdown",
    "pointermove",
    "pointerup",
    "mousedown",
    "mousemove",
    "mouseup",
    "touchstart",
    "touchmove",
    "touchend",
    "wheel",
  ].forEach((eventName) => {
    element.addEventListener(eventName, stop, { passive: true });
  });
}

function intensityToColor(intensity) {
  if (intensity <= 0.25) {
    return lerpColor([0.05, 0.1, 0.35], [0.1, 0.65, 1.0], intensity / 0.25);
  }
  if (intensity <= 0.5) {
    return lerpColor(
      [0.1, 0.65, 1.0],
      [0.1, 0.95, 0.45],
      (intensity - 0.25) / 0.25,
    );
  }
  if (intensity <= 0.75) {
    return lerpColor(
      [0.1, 0.95, 0.45],
      [1.0, 0.85, 0.15],
      (intensity - 0.5) / 0.25,
    );
  }
  return lerpColor(
    [1.0, 0.85, 0.15],
    [1.0, 0.25, 0.1],
    (intensity - 0.75) / 0.25,
  );
}

function lerpColor(a, b, t) {
  return [
    a[0] + (b[0] - a[0]) * t,
    a[1] + (b[1] - a[1]) * t,
    a[2] + (b[2] - a[2]) * t,
  ];
}

function onWindowResize() {
  renderer.setSize(window.innerWidth, window.innerHeight);
}

function animate() {
  requestAnimationFrame(animate);

  renderer.setClearColor(0x050505);
  renderer.setScissorTest(false);
  renderer.clear();
  renderer.setScissorTest(true);

  sensors.forEach((sensor) => {
    const rect = sensor.container.getBoundingClientRect();

    // Only render if visible on screen
    if (
      rect.bottom < 0 ||
      rect.top > renderer.domElement.clientHeight ||
      rect.right < 0 ||
      rect.left > renderer.domElement.clientWidth
    ) {
      return;
    }

    const width = rect.right - rect.left;
    const height = rect.bottom - rect.top;
    const left = rect.left;
    const bottom = renderer.domElement.clientHeight - rect.bottom;

    renderer.setViewport(left, bottom, width, height);
    renderer.setScissor(left, bottom, width, height);

    sensor.camera.aspect = width / height;
    sensor.camera.updateProjectionMatrix();

    sensor.controls.update();
    renderer.render(sensor.scene, sensor.camera);
  });
}
